#include "device/DeviceInfo.h"

#include <ArduinoJson.h>
#include <Arduino.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <stdio.h>
#include <string.h>

#include "config/Board.h"
#include "config/ConfigStore.h"
#include "log/Logger.h"

#ifndef SH_FW_VERSION
#define SH_FW_VERSION "0.0.0-dev"
#endif
#ifndef SH_HW_REVISION
#define SH_HW_REVISION "unknown"
#endif

namespace sh {
namespace {

constexpr const char* TAG = "dev";

char g_uuid[20] = {0};
char g_mac[18] = {0};
char g_short[5] = {0};
esp_reset_reason_t g_resetReason = ESP_RST_UNKNOWN;

const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power_on";
    case ESP_RST_EXT:      return "external_pin";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT:      return "other_watchdog";
    case ESP_RST_DEEPSLEEP:return "deep_sleep_wake";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
  }
}

}  // namespace

void DeviceInfo::begin() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  snprintf(g_mac, sizeof(g_mac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  snprintf(g_uuid, sizeof(g_uuid), "sh-%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  snprintf(g_short, sizeof(g_short), "%02X%02X", mac[4], mac[5]);

  g_resetReason = esp_reset_reason();

  SH_LOGI(TAG, "uuid=%s mac=%s fw=%s hw=%s", g_uuid, g_mac, SH_FW_VERSION,
          SH_HW_REVISION);
  SH_LOGI(TAG, "chip=%s rev=%u cores=%u flash=%u KB reset=%s", ESP.getChipModel(),
          static_cast<unsigned>(ESP.getChipRevision()),
          static_cast<unsigned>(ESP.getChipCores()),
          static_cast<unsigned>(ESP.getFlashChipSize() / 1024),
          resetReasonName(g_resetReason));

  if (lastResetWasBrownout()) {
    // Loudest possible warning: this is the 5 V supply / JD-VCC jumper problem
    // and it will otherwise be misdiagnosed as a firmware bug.
    SH_LOGE(TAG,
            "BROWNOUT RESET - the 5 V supply sagged. Check: JD-VCC jumper "
            "removed? separate 2 A supply on the coils? See docs/WIRING.md");
  }
}

const char* DeviceInfo::uuid() { return g_uuid; }
const char* DeviceInfo::macAddress() { return g_mac; }
const char* DeviceInfo::shortId() { return g_short; }
const char* DeviceInfo::firmwareVersion() { return SH_FW_VERSION; }
const char* DeviceInfo::hardwareRevision() { return SH_HW_REVISION; }
const char* DeviceInfo::chipModel() { return ESP.getChipModel(); }

uint8_t DeviceInfo::chipRevision() { return ESP.getChipRevision(); }
uint8_t DeviceInfo::chipCores() { return ESP.getChipCores(); }
uint32_t DeviceInfo::cpuFreqMhz() { return getCpuFrequencyMhz(); }
uint32_t DeviceInfo::flashSizeBytes() { return ESP.getFlashChipSize(); }
uint32_t DeviceInfo::sketchSizeBytes() { return ESP.getSketchSize(); }
uint32_t DeviceInfo::freeSketchSpaceBytes() { return ESP.getFreeSketchSpace(); }

uint32_t DeviceInfo::freeHeapBytes() { return ESP.getFreeHeap(); }
uint32_t DeviceInfo::minFreeHeapBytes() { return ESP.getMinFreeHeap(); }
uint32_t DeviceInfo::maxAllocHeapBytes() { return ESP.getMaxAllocHeap(); }

uint32_t DeviceInfo::heapFragmentationPct() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap == 0) return 0;
  const uint32_t largest = ESP.getMaxAllocHeap();
  if (largest >= freeHeap) return 0;
  return 100u - ((largest * 100u) / freeHeap);
}

uint64_t DeviceInfo::uptimeMs() { return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL); }

const char* DeviceInfo::resetReason() { return resetReasonName(g_resetReason); }

bool DeviceInfo::lastResetWasBrownout() { return g_resetReason == ESP_RST_BROWNOUT; }

size_t DeviceInfo::toRegistrationJson(char* out, size_t cap) {
  JsonDocument doc;
  doc["uuid"] = g_uuid;
  doc["mac"] = g_mac;
  doc["name"] = ConfigStore::device().deviceName;
  doc["firmwareVersion"] = SH_FW_VERSION;
  doc["hardwareRevision"] = SH_HW_REVISION;
  doc["relayCount"] = board::kChannelCount;
  doc["switchCount"] = board::kChannelCount;

  JsonObject chip = doc["chip"].to<JsonObject>();
  chip["model"] = ESP.getChipModel();
  chip["revision"] = ESP.getChipRevision();
  chip["cores"] = ESP.getChipCores();
  chip["cpuMhz"] = getCpuFrequencyMhz();
  chip["sdk"] = ESP.getSdkVersion();

  JsonObject mem = doc["memory"].to<JsonObject>();
  mem["freeHeap"] = ESP.getFreeHeap();
  mem["minFreeHeap"] = ESP.getMinFreeHeap();
  mem["maxAlloc"] = ESP.getMaxAllocHeap();
  mem["flashSize"] = ESP.getFlashChipSize();
  mem["sketchSize"] = ESP.getSketchSize();
  mem["freeSketchSpace"] = ESP.getFreeSketchSpace();

  doc["uptimeMs"] = uptimeMs();
  doc["resetReason"] = resetReasonName(g_resetReason);

  // What this device can do. The backend uses this to decide which UI to show
  // and which future device classes it may become (plan: Future Expansion).
  JsonArray caps = doc["capabilities"].to<JsonArray>();
  caps.add("relay.switch");
  caps.add("relay.group");
  caps.add("relay.timer");
  caps.add("switch.physical");
  caps.add("switch.latching");
  caps.add("switch.momentary");
  caps.add("state.restore");
  caps.add("schedule.local");
  caps.add("automation.local");
  caps.add("api.local.rest");
  caps.add("api.local.websocket");
  caps.add("mqtt.qos1");
  caps.add("mqtt.lwt");
  caps.add("alexa.local.hue");
  caps.add("ota.https.rollback");
  caps.add("diagnostics.heartbeat");

  return serializeJson(doc, out, cap);
}

size_t DeviceInfo::toDiagnosticsJson(char* out, size_t cap) {
  JsonDocument doc;
  doc["uuid"] = g_uuid;
  doc["fw"] = SH_FW_VERSION;
  doc["uptimeMs"] = uptimeMs();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["minFreeHeap"] = ESP.getMinFreeHeap();
  doc["maxAlloc"] = ESP.getMaxAllocHeap();
  doc["heapFragPct"] = heapFragmentationPct();
  doc["cpuMhz"] = getCpuFrequencyMhz();
  doc["resetReason"] = resetReasonName(g_resetReason);
  doc["brownout"] = lastResetWasBrownout();
  doc["rssi"] = WiFi.isConnected() ? WiFi.RSSI() : 0;
  doc["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : String("");
  doc["wifi"] = WiFi.isConnected();
  // Internal temperature: uncalibrated on WROOM-32 and reads high. Reported
  // for trend only - never treat it as an ambient temperature.
  doc["chipTempC"] = static_cast<int>(temperatureRead());
  return serializeJson(doc, out, cap);
}

}  // namespace sh
