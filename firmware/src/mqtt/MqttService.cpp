#include "mqtt/MqttService.h"

#include <ArduinoJson.h>
#include <Arduino.h>
#include <esp_crt_bundle.h>
#include <mqtt_client.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "config/Board.h"
#include "config/ConfigStore.h"
#include "core/EventBus.h"
#include "device/DeviceInfo.h"
#include "log/Logger.h"
#include "net/TimeService.h"
#include "relay/RelayManager.h"
#include "security/Security.h"

namespace sh {
namespace {

constexpr const char* TAG = "mqtt";
constexpr int kQos1 = 1;

esp_mqtt_client_handle_t g_client = nullptr;
bool     g_started = false;
volatile bool g_connected = false;
uint32_t g_reconnects = 0;
uint32_t g_publishes = 0;

char g_base[112] = {0};
char g_uri[128] = {0};
char g_lwtTopic[128] = {0};

void buildTopics() {
  const DeviceConfig& cfg = ConfigStore::device();
  const char* home = cfg.homeId[0] ? cfg.homeId : "unclaimed";
  snprintf(g_base, sizeof(g_base), "smarthome/%s/%s", home, DeviceInfo::uuid());
  snprintf(g_lwtTopic, sizeof(g_lwtTopic), "%s/status", g_base);
  snprintf(g_uri, sizeof(g_uri), "%s://%s:%u", cfg.mqttTls ? "mqtts" : "mqtt",
           cfg.mqttHost, static_cast<unsigned>(cfg.mqttPort));
}

/// Non-blocking publish. Called from the EventBus (relay task), where blocking
/// would break the switch latency guarantee, so we enqueue rather than send.
bool enqueue(const char* topic, const char* payload, bool retain, int qos = kQos1) {
  if (!g_client || !g_connected) return false;
  const int id = esp_mqtt_client_enqueue(g_client, topic, payload,
                                         static_cast<int>(strlen(payload)), qos,
                                         retain ? 1 : 0, /*store=*/true);
  if (id < 0) {
    SH_LOGW(TAG, "enqueue failed for %s", topic);
    return false;
  }
  g_publishes++;
  return true;
}

void publishChannelState(uint8_t ch) {
  char topic[144];
  snprintf(topic, sizeof(topic), "%s/relay/%u/state", g_base,
           static_cast<unsigned>(ch));
  char payload[288];
  RelayManager::channelToJson(ch, payload, sizeof(payload));
  enqueue(topic, payload, /*retain=*/true);
}

void publishStatus(bool online) {
  char payload[192];
  JsonDocument doc;
  doc["online"] = online;
  doc["uuid"] = DeviceInfo::uuid();
  doc["fw"] = DeviceInfo::firmwareVersion();
  doc["ts"] = TimeService::nowMsOrUptime();
  serializeJson(doc, payload, sizeof(payload));
  enqueue(g_lwtTopic, payload, /*retain=*/true);
}

void publishRegistration() {
  char topic[144];
  snprintf(topic, sizeof(topic), "%s/register", g_base);

  char* payload = static_cast<char*>(malloc(2048));
  if (!payload) return;
  const size_t n = DeviceInfo::toRegistrationJson(payload, 2048);
  if (n == 0 || n >= 2048) {
    free(payload);
    return;
  }

  // Sign it. The backend can then tell a real device from someone replaying a
  // registration they sniffed, and the counter stops the replay itself.
  if (Security::hasApiKey()) {
    const uint32_t counter = Security::nextCounter();
    char sig[65];
    if (Security::signRequest("REGISTER", topic, payload, counter, sig, sizeof(sig))) {
      JsonDocument doc;
      if (!deserializeJson(doc, payload)) {
        doc["counter"] = counter;
        doc["sig"] = sig;
        serializeJson(doc, payload, 2048);
      }
    }
  } else {
    // Unclaimed: announce the claim code so the app can offer "adopt this
    // device" without the user hunting for the enclosure label.
    JsonDocument doc;
    if (!deserializeJson(doc, payload)) {
      doc["claimCode"] = Security::claimCode();
      doc["claimed"] = false;
      serializeJson(doc, payload, 2048);
    }
  }

  enqueue(topic, payload, /*retain=*/false);
  free(payload);
  SH_LOGI(TAG, "registration published");
}

void subscribeAll() {
  char t[144];
  snprintf(t, sizeof(t), "%s/relay/+/set", g_base);
  esp_mqtt_client_subscribe(g_client, t, kQos1);
  snprintf(t, sizeof(t), "%s/cmd", g_base);
  esp_mqtt_client_subscribe(g_client, t, kQos1);
  snprintf(t, sizeof(t), "%s/config/set", g_base);
  esp_mqtt_client_subscribe(g_client, t, kQos1);
  SH_LOGI(TAG, "subscribed under %s", g_base);
}

Action parseAction(const char* s, bool& valid) {
  valid = true;
  if (!s) {
    valid = false;
    return Action::Toggle;
  }
  if (strcasecmp(s, "on") == 0 || strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0)
    return Action::On;
  if (strcasecmp(s, "off") == 0 || strcmp(s, "0") == 0 || strcasecmp(s, "false") == 0)
    return Action::Off;
  if (strcasecmp(s, "toggle") == 0) return Action::Toggle;
  valid = false;
  return Action::Toggle;
}

void handleRelaySet(const char* topic, const char* payload) {
  // .../relay/<what>/set
  const char* p = strstr(topic, "/relay/");
  if (!p) return;
  p += 7;
  char what[12] = {0};
  size_t i = 0;
  while (*p && *p != '/' && i < sizeof(what) - 1) what[i++] = *p++;

  // Payload may be bare ("on") or JSON ({"action":"on","rev":42,...}).
  Action action = Action::Toggle;
  bool valid = false;
  uint32_t rev = 0;
  bool hasRev = false;
  uint32_t seconds = 0;

  if (payload[0] == '{') {
    JsonDocument doc;
    if (deserializeJson(doc, payload)) {
      SH_LOGW(TAG, "bad json on %s", topic);
      return;
    }
    // Replay protection on inbound commands, when the backend signs them.
    if (doc["counter"].is<uint32_t>()) {
      if (!Security::acceptInboundCounter(doc["counter"].as<uint32_t>())) return;
    }
    if (doc["action"].is<const char*>()) {
      action = parseAction(doc["action"], valid);
    } else if (doc["state"].is<bool>()) {
      action = doc["state"].as<bool>() ? Action::On : Action::Off;
      valid = true;
    }
    if (doc["rev"].is<uint32_t>()) {
      rev = doc["rev"];
      hasRev = true;
    }
    seconds = doc["seconds"] | 0u;
  } else {
    action = parseAction(payload, valid);
  }
  if (!valid) {
    SH_LOGW(TAG, "unrecognised action on %s: %s", topic, payload);
    return;
  }

  if (strcmp(what, "all") == 0) {
    RelayManager::commandAll(action, Source::Cloud);
    return;
  }
  const int ch = atoi(what);
  if (ch < 0 || ch >= board::kChannelCount) return;

  if (seconds > 0) {
    RelayManager::setAutoOff(static_cast<uint8_t>(ch), seconds, Source::Cloud);
  } else if (hasRev) {
    RelayManager::commandWithRev(static_cast<uint8_t>(ch), action, Source::Cloud, rev);
  } else {
    RelayManager::command(static_cast<uint8_t>(ch), action, Source::Cloud);
  }
}

void handleCommand(const char* payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;
  if (doc["counter"].is<uint32_t>() &&
      !Security::acceptInboundCounter(doc["counter"].as<uint32_t>())) {
    return;
  }

  const char* cmd = doc["cmd"] | "";
  SH_LOGI(TAG, "cloud command: %s", cmd);

  if (strcmp(cmd, "snapshot") == 0) {
    MqttService::publishFullState();
  } else if (strcmp(cmd, "diag") == 0) {
    MqttService::publishDiagnostics();
  } else if (strcmp(cmd, "reboot") == 0) {
    RelayManager::flush();
    esp_restart();
  } else if (strcmp(cmd, "identify") == 0) {
    Event e;
    e.type = EventType::Heartbeat;
    EventBus::publish(e);
  } else if (strcmp(cmd, "ota") == 0) {
    Event e;
    e.type = EventType::OtaStarted;
    e.source = Source::Cloud;
    EventBus::publish(e);
  } else if (strcmp(cmd, "factory-reset") == 0) {
    // Cloud-initiated wipe must prove it holds the device key, otherwise a
    // spoofed message could brick a fleet.
    const char* sig = doc["sig"];
    char expect[65];
    if (sig && Security::signRequest("FACTORY", g_base, "",
                                     doc["counter"] | 0u, expect, sizeof(expect)) &&
        Security::secureEquals(sig, expect)) {
      Event e;
      e.type = EventType::FactoryReset;
      e.source = Source::Cloud;
      EventBus::publish(e);
    } else {
      SH_LOGE(TAG, "rejected unsigned factory-reset");
    }
  }
}

void onMqttEvent(void* /*handlerArgs*/, esp_event_base_t /*base*/, int32_t id,
                 void* eventData) {
  auto* ev = static_cast<esp_mqtt_event_handle_t>(eventData);

  switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED: {
      g_connected = true;
      g_reconnects++;
      SH_LOGI(TAG, "connected to %s", g_uri);

      subscribeAll();
      publishStatus(true);
      publishRegistration();
      MqttService::publishFullState();

      Event e;
      e.type = EventType::MqttConnected;
      EventBus::publish(e);
      break;
    }
    case MQTT_EVENT_DISCONNECTED: {
      if (g_connected) SH_LOGW(TAG, "disconnected");
      g_connected = false;
      Event e;
      e.type = EventType::MqttDisconnected;
      EventBus::publish(e);
      break;
    }
    case MQTT_EVENT_DATA: {
      if (ev->topic_len <= 0 || ev->data_len < 0) break;
      // esp_mqtt gives unterminated slices of its own buffer.
      char topic[160];
      const int tl = ev->topic_len < static_cast<int>(sizeof(topic)) - 1
                         ? ev->topic_len
                         : static_cast<int>(sizeof(topic)) - 1;
      memcpy(topic, ev->topic, tl);
      topic[tl] = '\0';

      if (ev->data_len > 1024) {
        SH_LOGW(TAG, "payload too large on %s (%d bytes)", topic, ev->data_len);
        break;
      }
      char payload[1025];
      memcpy(payload, ev->data, ev->data_len);
      payload[ev->data_len] = '\0';

      SH_LOGD(TAG, "rx %s: %s", topic, payload);

      if (strstr(topic, "/relay/") && strstr(topic, "/set")) {
        handleRelaySet(topic, payload);
      } else if (strstr(topic, "/cmd")) {
        handleCommand(payload);
      } else if (strstr(topic, "/config/set")) {
        char err[96];
        if (ConfigStore::applyJson(payload, err, sizeof(err))) {
          ConfigStore::save();
        } else {
          SH_LOGW(TAG, "cloud config rejected: %s", err);
        }
      }
      break;
    }
    case MQTT_EVENT_ERROR:
      if (ev->error_handle) {
        SH_LOGE(TAG, "error type=%d tls=0x%x sock_errno=%d",
                static_cast<int>(ev->error_handle->error_type),
                ev->error_handle->esp_tls_last_esp_err,
                ev->error_handle->esp_transport_sock_errno);
      }
      break;
    default:
      break;
  }
}

/// Mirrors relay changes onto MQTT. Non-blocking, per the EventBus contract.
void onBusEvent(const Event& e, void* /*ctx*/) {
  if (!g_connected) return;
  if (e.type == EventType::RelayChanged) {
    publishChannelState(e.channel);
  }
}

}  // namespace

// ---------------------------------------------------------------------------

void MqttService::begin() {
  buildTopics();
  EventBus::subscribe(onBusEvent, nullptr, "mqtt");
}

bool MqttService::isEnabled() {
  const DeviceConfig& cfg = ConfigStore::device();
  return cfg.cloudEnabled && cfg.mqttHost[0] != '\0';
}

void MqttService::connect() {
  if (!isEnabled()) {
    SH_LOGI(TAG, "cloud disabled or no broker configured, staying local-only");
    return;
  }
  buildTopics();

  if (g_started) {
    esp_mqtt_client_reconnect(g_client);
    return;
  }

  const DeviceConfig& cfg = ConfigStore::device();

  char lwtPayload[128];
  {
    JsonDocument doc;
    doc["online"] = false;
    doc["uuid"] = DeviceInfo::uuid();
    doc["reason"] = "lwt";
    serializeJson(doc, lwtPayload, sizeof(lwtPayload));
  }

  esp_mqtt_client_config_t mc = {};
  mc.broker.address.uri = g_uri;
  mc.credentials.client_id = DeviceInfo::uuid();
  if (cfg.mqttUser[0]) mc.credentials.username = cfg.mqttUser;
  if (cfg.mqttPass[0]) mc.credentials.authentication.password = cfg.mqttPass;

  mc.session.last_will.topic = g_lwtTopic;
  mc.session.last_will.msg = lwtPayload;
  mc.session.last_will.msg_len = static_cast<int>(strlen(lwtPayload));
  mc.session.last_will.qos = kQos1;
  mc.session.last_will.retain = 1;
  mc.session.keepalive = 30;
  mc.session.disable_clean_session = false;

  mc.network.reconnect_timeout_ms = 5000;
  mc.network.timeout_ms = 10000;
  mc.task.stack_size = 6144;
  mc.buffer.size = 2048;
  mc.buffer.out_size = 2048;

  if (cfg.mqttTls) {
    // The bundled Mozilla root store rather than one pinned CA: a pinned root
    // that expires would permanently cut the device off from the very channel
    // needed to fix it (Loophole #18). The bundle ships with the firmware and
    // is refreshed by an OTA.
    mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  }

  g_client = esp_mqtt_client_init(&mc);
  if (!g_client) {
    SH_LOGE(TAG, "client init failed");
    return;
  }
  esp_mqtt_client_register_event(g_client, MQTT_EVENT_ANY, onMqttEvent, nullptr);

  const esp_err_t err = esp_mqtt_client_start(g_client);
  if (err != ESP_OK) {
    SH_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
    return;
  }
  g_started = true;
  SH_LOGI(TAG, "connecting to %s (tls=%s)", g_uri, cfg.mqttTls ? "yes" : "no");
}

void MqttService::disconnect() {
  if (!g_client || !g_started) return;
  publishStatus(false);
  esp_mqtt_client_stop(g_client);
  g_connected = false;
  g_started = false;
}

bool MqttService::isConnected() { return g_connected; }
uint32_t MqttService::reconnectCount() { return g_reconnects; }
uint32_t MqttService::publishCount() { return g_publishes; }
const char* MqttService::baseTopic() { return g_base; }

void MqttService::publishFullState() {
  if (!g_connected) return;

  char topic[144];
  snprintf(topic, sizeof(topic), "%s/state", g_base);
  char* buf = static_cast<char*>(malloc(3072));
  if (buf) {
    RelayManager::toJson(buf, 3072);
    enqueue(topic, buf, /*retain=*/true);
    free(buf);
  }
  // Per-channel retained topics too: they are what a lightweight subscriber
  // (or the Alexa bridge in the backend) actually watches.
  for (uint8_t ch = 0; ch < board::kChannelCount; ++ch) publishChannelState(ch);
}

void MqttService::publishDiagnostics() {
  if (!g_connected) return;
  char topic[144];
  snprintf(topic, sizeof(topic), "%s/diag", g_base);
  char* buf = static_cast<char*>(malloc(1024));
  if (!buf) return;
  DeviceInfo::toDiagnosticsJson(buf, 1024);
  enqueue(topic, buf, /*retain=*/false);
  free(buf);
}

}  // namespace sh
