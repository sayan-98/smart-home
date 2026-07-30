#include "core/App.h"

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "alexa/AlexaLocal.h"
#include "automation/AutomationEngine.h"
#include "config/Board.h"
#include "config/ConfigStore.h"
#include "core/EventBus.h"
#include "core/StatusLed.h"
#include "device/DeviceInfo.h"
#include "diag/Diagnostics.h"
#include "http/LocalApi.h"
#include "log/Logger.h"
#include "mqtt/MqttService.h"
#include "net/TimeService.h"
#include "net/WiFiService.h"
#include "ota/OtaService.h"
#include "relay/RelayManager.h"
#include "scheduler/Scheduler.h"
#include "security/Security.h"
#include "storage/NvsStore.h"
#include "switchio/SwitchScanner.h"

#ifndef SH_FW_VERSION
#define SH_FW_VERSION "0.0.0-dev"
#endif
#ifndef SH_LOG_LEVEL
#define SH_LOG_LEVEL 4
#endif

namespace sh {
namespace {

constexpr const char* TAG = "app";
constexpr const char* kBootCountKey = "boots";

/// How long the new firmware must stay online before we cancel the OTA
/// rollback. Long enough to prove the network actually works, short enough
/// that a genuine update is confirmed within a minute.
constexpr uint32_t kImageProveMs = 45000;

uint32_t g_bootCount = 0;
uint32_t g_onlineSinceMs = 0;
bool     g_factoryResetRequested = false;
bool     g_mqttStartRequested = false;

}  // namespace

void App::boot() {
  // --- 1. voice -----------------------------------------------------------
  Logger::begin(115200, static_cast<LogLevel>(SH_LOG_LEVEL));
  delay(80);  // let the USB serial enumerate so the banner is not lost
  SH_LOGI(TAG, "");
  SH_LOGI(TAG, "=== Smart Home OS %s ===", SH_FW_VERSION);

  EventBus::begin();
  StatusLed::begin();

  // 15 s, not the 5 s default: a TLS handshake on a weak link plus a blocking
  // Wi-Fi scan can legitimately hold a task for several seconds, and a
  // watchdog that fires on healthy work is worse than none.
  {
    esp_task_wdt_config_t wdt = {};
    wdt.timeout_ms = 15000;
    wdt.idle_core_mask = 0;
    wdt.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&wdt) == ESP_ERR_INVALID_STATE) {
      esp_task_wdt_init(&wdt);
    }
  }

  // --- 2. identity and configuration --------------------------------------
  DeviceInfo::begin();
  NvsStore::begin();  // create namespaces before anything reads them
  ConfigStore::begin();
  Logger::setLevel(static_cast<LogLevel>(ConfigStore::device().logLevel));
  Security::begin();

  g_bootCount = NvsStore::getU32(nvsns::kSystem, kBootCountKey, 0) + 1;
  NvsStore::putU32(nvsns::kSystem, kBootCountKey, g_bootCount);
  SH_LOGI(TAG, "boot #%lu", static_cast<unsigned long>(g_bootCount));

  OtaService::begin();

  // --- 3. RELAYS FIRST ----------------------------------------------------
  // Nothing slow may run before this. The external pull-ups are what actually
  // hold the coils off during reset (docs/WIRING.md); getting here quickly is
  // what keeps that window short afterwards.
  RelayManager::begin();
  SwitchScanner::begin();

  RelayManager::startTask();
  SwitchScanner::startTask();
  SH_LOGI(TAG, "relay + switch engine live - wall switches work from here on");

  // --- 4. everything else -------------------------------------------------
  TimeService::begin();
  Scheduler::begin();
  AutomationEngine::begin();
  AlexaLocal::begin();
  MqttService::begin();
  Diagnostics::begin();

  EventBus::subscribe(&App::onEvent, nullptr, "app");

  WiFiService::begin();
  LocalApi::begin();

  WiFiService::startTask();
  LocalApi::startTask();
  Scheduler::startTask();
  AutomationEngine::startTask();
  Diagnostics::startTask();

  StatusLed::set(WiFiService::savedNetworkCount() == 0 ? LedPattern::Provisioning
                                                       : LedPattern::Connecting);

  SH_LOGI(TAG, "boot complete: %u tasks, %u KB heap free",
          static_cast<unsigned>(uxTaskGetNumberOfTasks()),
          static_cast<unsigned>(DeviceInfo::freeHeapBytes() / 1024));

  if (!Security::isClaimed()) {
    SH_LOGW(TAG, "device is UNCLAIMED. Claim code: %s", Security::claimCode());
  }
}

void App::onEvent(const Event& e, void* /*ctx*/) {
  switch (e.type) {
    case EventType::WifiConnected:
      g_onlineSinceMs = millis();
      // Hue discovery only works once we have a routable address.
      AlexaLocal::onNetworkUp();
      // MQTT is started from tick(), not here: this handler runs on the Wi-Fi
      // task and esp_mqtt_client_start does real work.
      g_mqttStartRequested = true;
      updateLedPolicy();
      break;

    case EventType::WifiDisconnected:
      g_onlineSinceMs = 0;
      AlexaLocal::onNetworkDown();
      updateLedPolicy();
      break;

    case EventType::MqttConnected:
    case EventType::MqttDisconnected:
      updateLedPolicy();
      break;

    case EventType::DeviceClaimed:
      SH_LOGI(TAG, "claimed - bringing up the cloud connection");
      g_mqttStartRequested = true;
      break;

    case EventType::ConfigChanged:
      Logger::setLevel(static_cast<LogLevel>(ConfigStore::device().logLevel));
      break;

    case EventType::FactoryReset:
      // Do not wipe from inside an event handler - the caller may be an HTTP
      // task mid-response. Flag it and let tick() do it.
      g_factoryResetRequested = true;
      break;

    case EventType::OtaStarted:
      StatusLed::set(LedPattern::Ota);
      break;

    case EventType::OtaFailed:
      updateLedPolicy();
      break;

    default:
      break;
  }
}

void App::updateLedPolicy() {
  if (OtaService::state() == OtaState::Downloading ||
      OtaService::state() == OtaState::Applying) {
    StatusLed::set(LedPattern::Ota);
    return;
  }
  if (WiFiService::isApActive() && !WiFiService::isConnected()) {
    StatusLed::set(LedPattern::Provisioning);
    return;
  }
  if (!WiFiService::isConnected()) {
    StatusLed::set(LedPattern::Connecting);
    return;
  }
  if (Diagnostics::heapCritical()) {
    StatusLed::set(LedPattern::Fault);
    return;
  }
  StatusLed::set(MqttService::isConnected() ? LedPattern::OnlineCloud
                                            : LedPattern::OnlineLocal);
}

void App::performFactoryReset() {
  SH_LOGE(TAG, "FACTORY RESET - wiping configuration, credentials and pairing");

  // Sockets off first. A reset that leaves an appliance energised while the
  // device forgets how to talk to anything is the wrong failure mode.
  RelayManager::safeShutdown(Source::Factory);
  MqttService::disconnect();

  WiFiService::forgetAll();
  Security::unclaim();
  NvsStore::factoryWipe();
  ConfigStore::resetToDefaults();

  StatusLed::set(LedPattern::Fault);
  delay(1500);
  esp_restart();
}

void App::tick() {
  const uint32_t now = millis();

  StatusLed::tick(now);

  if (g_factoryResetRequested) {
    g_factoryResetRequested = false;
    performFactoryReset();  // does not return
  }

  if (g_mqttStartRequested && WiFiService::isConnected()) {
    g_mqttStartRequested = false;
    MqttService::connect();
  }

  // Confirm a freshly installed image only after it has held a network
  // connection for a while. Marking it valid at boot would defeat rollback for
  // exactly the failure that matters most: an image that boots but cannot get
  // online.
  if (OtaService::isPendingVerify() && g_onlineSinceMs != 0 &&
      (now - g_onlineSinceMs) > kImageProveMs) {
    const bool cloudNeeded = MqttService::isEnabled();
    if (!cloudNeeded || MqttService::isConnected()) {
      OtaService::markCurrentImageValid();
    }
  }

  static uint32_t lastLed = 0;
  if (now - lastLed > 2000) {
    lastLed = now;
    updateLedPolicy();
  }

  delay(20);
}

uint32_t App::bootCount() { return g_bootCount; }

}  // namespace sh
