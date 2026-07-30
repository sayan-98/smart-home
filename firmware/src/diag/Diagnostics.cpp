#include "diag/Diagnostics.h"

#include <ArduinoJson.h>
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "automation/AutomationEngine.h"
#include "config/Board.h"
#include "core/EventBus.h"
#include "device/DeviceInfo.h"
#include "log/Logger.h"
#include "mqtt/MqttService.h"
#include "net/TimeService.h"
#include "net/WiFiService.h"
#include "ota/OtaService.h"
#include "relay/RelayManager.h"
#include "scheduler/Scheduler.h"
#include "storage/NvsStore.h"

namespace sh {
namespace {

constexpr const char* TAG = "diag";

/// Below this, a TLS handshake will start failing. Warn before that happens
/// rather than after, when the symptom is "MQTT randomly stopped working".
constexpr uint32_t kHeapWarnBytes = 40000;
constexpr uint32_t kHeapCriticalBytes = 25000;

/// A link this weak drops packets and stalls reconnects long before it fully
/// disassociates, which reads as "the switch is laggy".
constexpr int32_t kRssiWarnDbm = -78;

uint32_t g_beats = 0;
uint32_t g_lowHeapEvents = 0;
uint32_t g_minHeap = 0xFFFFFFFF;
bool     g_heapCritical = false;
bool     g_warnedRssi = false;
TaskHandle_t g_task = nullptr;

}  // namespace

void Diagnostics::begin() {
  g_minHeap = DeviceInfo::freeHeapBytes();
  SH_LOGI(TAG, "heartbeat every %u s, heap now %u KB",
          static_cast<unsigned>(kHeartbeatMs / 1000),
          static_cast<unsigned>(g_minHeap / 1024));
}

size_t Diagnostics::toJson(char* out, size_t cap) {
  JsonDocument doc;
  doc["uuid"] = DeviceInfo::uuid();
  doc["fw"] = DeviceInfo::firmwareVersion();
  doc["partition"] = OtaService::runningPartition();
  doc["pendingVerify"] = OtaService::isPendingVerify();
  doc["uptimeMs"] = DeviceInfo::uptimeMs();

  JsonObject mem = doc["memory"].to<JsonObject>();
  mem["freeHeap"] = DeviceInfo::freeHeapBytes();
  mem["minFreeHeap"] = DeviceInfo::minFreeHeapBytes();
  mem["minSeen"] = g_minHeap;
  mem["maxAlloc"] = DeviceInfo::maxAllocHeapBytes();
  mem["fragPct"] = DeviceInfo::heapFragmentationPct();
  mem["critical"] = g_heapCritical;
  mem["nvsFreeEntries"] = NvsStore::freeEntries();

  JsonObject net = doc["network"].to<JsonObject>();
  net["wifi"] = WiFiService::isConnected();
  net["ssid"] = WiFiService::isConnected() ? WiFiService::ssid() : "";
  net["rssi"] = WiFiService::rssi();
  net["ip"] = WiFiService::localIp().toString();
  net["ap"] = WiFiService::isApActive();
  net["mqtt"] = MqttService::isConnected();
  net["mqttReconnects"] = MqttService::reconnectCount();
  net["mqttPublishes"] = MqttService::publishCount();

  JsonObject t = doc["time"].to<JsonObject>();
  char iso[40];
  TimeService::formatIso(iso, sizeof(iso));
  t["iso"] = iso;
  t["synced"] = TimeService::isSynced();
  t["secondsSinceSync"] = TimeService::secondsSinceSync();

  JsonObject health = doc["health"].to<JsonObject>();
  health["resetReason"] = DeviceInfo::resetReason();
  // Called out explicitly because it is the single most misdiagnosed fault on
  // this hardware - it looks exactly like a firmware crash.
  health["brownout"] = DeviceInfo::lastResetWasBrownout();
  health["cpuMhz"] = DeviceInfo::cpuFreqMhz();
  health["chipTempC"] = static_cast<int>(temperatureRead());
  health["heartbeats"] = g_beats;
  health["lowHeapEvents"] = g_lowHeapEvents;

  JsonObject logic = doc["logic"].to<JsonObject>();
  logic["relayMask"] = RelayManager::stateMask();
  logic["relayRev"] = RelayManager::globalRevision();
  logic["schedules"] = Scheduler::ruleCount();
  logic["schedulesFired"] = Scheduler::firedCount();
  logic["automations"] = AutomationEngine::ruleCount();
  logic["automationsFired"] = AutomationEngine::firedCount();
  logic["automationsSuppressed"] = AutomationEngine::suppressedCount();

  return serializeJson(doc, out, cap);
}

uint32_t Diagnostics::heartbeatCount() { return g_beats; }
uint32_t Diagnostics::lowHeapEvents() { return g_lowHeapEvents; }
uint32_t Diagnostics::minHeapSeen() { return g_minHeap; }
bool Diagnostics::heapCritical() { return g_heapCritical; }

void Diagnostics::startTask() {
  if (g_task) return;
  xTaskCreatePinnedToCore(&Diagnostics::taskEntry, "diag", 4096, nullptr, 1,
                          &g_task, 0);
}

void Diagnostics::taskEntry(void* /*arg*/) {
  esp_task_wdt_add(nullptr);
  uint32_t lastBeat = 0;

  for (;;) {
    esp_task_wdt_reset();

    const uint32_t heap = DeviceInfo::freeHeapBytes();
    if (heap < g_minHeap) g_minHeap = heap;

    if (heap < kHeapCriticalBytes) {
      if (!g_heapCritical) {
        g_heapCritical = true;
        g_lowHeapEvents++;
        SH_LOGE(TAG, "heap critical: %u bytes free - TLS will start failing", heap);
      }
    } else if (heap < kHeapWarnBytes) {
      if (!g_heapCritical) {
        g_lowHeapEvents++;
        SH_LOGW(TAG, "heap low: %u bytes free", heap);
      }
    } else {
      g_heapCritical = false;
    }

    if (WiFiService::isConnected()) {
      const int32_t rssi = WiFiService::rssi();
      if (rssi < kRssiWarnDbm && !g_warnedRssi) {
        g_warnedRssi = true;
        SH_LOGW(TAG, "weak Wi-Fi: %ld dBm - expect dropped commands",
                static_cast<long>(rssi));
      } else if (rssi > kRssiWarnDbm + 6) {
        g_warnedRssi = false;
      }
    }

    if (millis() - lastBeat >= kHeartbeatMs) {
      lastBeat = millis();
      g_beats++;

      MqttService::publishDiagnostics();

      Event e;
      e.type = EventType::Heartbeat;
      e.value = static_cast<int32_t>(heap);
      e.tsMs = TimeService::nowMsOrUptime();
      e.tsSynced = TimeService::isSynced();
      EventBus::publish(e);

      SH_LOGD(TAG, "heartbeat #%lu heap=%u rssi=%ld mqtt=%s",
              static_cast<unsigned long>(g_beats), heap,
              static_cast<long>(WiFiService::rssi()),
              MqttService::isConnected() ? "up" : "down");
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

}  // namespace sh
