#include "http/LocalApi.h"

#include <ArduinoJson.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

#include "alexa/AlexaLocal.h"
#include "automation/AutomationEngine.h"
#include "config/ConfigStore.h"
#include "core/EventBus.h"
#include "ota/OtaService.h"
#include "scheduler/Scheduler.h"
#include "device/DeviceInfo.h"
#include "http/PortalPage.h"
#include "log/Logger.h"
#include "mqtt/MqttService.h"
#include "net/TimeService.h"
#include "net/WiFiService.h"
#include "relay/RelayManager.h"
#include "security/Security.h"
#include "storage/NvsStore.h"
#include "switchio/SwitchScanner.h"

namespace sh {
namespace {

constexpr const char* TAG = "http";
constexpr uint16_t kPort = 80;
constexpr size_t kMaxBody = 4096;
constexpr size_t kJsonBuf = 4096;
constexpr uint32_t kSessionTtlMs = 12UL * 60UL * 60UL * 1000UL;  // 12 h

AsyncWebServer  g_server(kPort);
AsyncWebSocket  g_ws("/ws");
DNSServer       g_dns;
TaskHandle_t    g_task = nullptr;
QueueHandle_t   g_events = nullptr;
bool            g_running = false;
bool            g_dnsUp = false;

char     g_sessionToken[33] = {0};
uint32_t g_sessionIssuedMs = 0;

/// Compact copy of a relay change, queued for the broadcast task so the
/// EventBus handler stays non-blocking.
struct WsEvent {
  uint8_t  channel;
  bool     state;
  Source   source;
  uint32_t rev;
  bool     snapshot;  // true = send the whole state instead
};

// --- helpers --------------------------------------------------------------

void sendJson(AsyncWebServerRequest* req, int code, const char* json) {
  // charset=utf-8 is not optional here. Wi-Fi SSIDs routinely contain
  // non-ASCII characters - a phone hotspot named "Sayan's iPhone" uses a curly
  // apostrophe - and without the charset many clients decode the response as
  // Latin-1, turning one character into three and making the name unmatchable.
  AsyncWebServerResponse* res =
      req->beginResponse(code, "application/json; charset=utf-8", json);
  res->addHeader("Cache-Control", "no-store");
  req->send(res);
}

void sendError(AsyncWebServerRequest* req, int code, const char* message) {
  char buf[192];
  JsonDocument doc;
  doc["error"] = message;
  serializeJson(doc, buf, sizeof(buf));
  sendJson(req, code, buf);
}

void sendOk(AsyncWebServerRequest* req) { sendJson(req, 200, "{\"ok\":true}"); }

const char* bodyOf(AsyncWebServerRequest* req) {
  return req->_tempObject ? static_cast<const char*>(req->_tempObject) : "";
}

/// Accumulates a request body into request-scoped memory. ESPAsyncWebServer
/// frees _tempObject when the request is destroyed, so there is no leak.
void collectBody(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                 size_t index, size_t total) {
  if (index == 0) {
    if (total > kMaxBody) {
      SH_LOGW(TAG, "body too large (%u bytes)", static_cast<unsigned>(total));
      return;
    }
    if (req->_tempObject) free(req->_tempObject);
    req->_tempObject = malloc(total + 1);
    if (req->_tempObject) static_cast<char*>(req->_tempObject)[0] = '\0';
  }
  if (!req->_tempObject) return;
  memcpy(static_cast<char*>(req->_tempObject) + index, data, len);
  if (index + len >= total) static_cast<char*>(req->_tempObject)[total] = '\0';
}

bool sessionValid() {
  if (g_sessionToken[0] == '\0') return false;
  return (millis() - g_sessionIssuedMs) < kSessionTtlMs;
}

/// True when the caller may perform a mutating request.
bool authorized(AsyncWebServerRequest* req) {
  // An unclaimed device has to be reachable - that is how it gets set up.
  if (!Security::isClaimed()) return true;

  if (req->hasHeader("X-API-Key")) {
    const AsyncWebHeader* h = req->getHeader("X-API-Key");
    if (h && Security::secureEquals(h->value().c_str(), Security::apiKey())) return true;
  }
  if (sessionValid() && req->hasHeader("Cookie")) {
    const AsyncWebHeader* c = req->getHeader("Cookie");
    if (c && c->value().indexOf(g_sessionToken) >= 0) return true;
  }
  return false;
}

bool requireAuth(AsyncWebServerRequest* req) {
  if (authorized(req)) return true;
  sendError(req, 401, "authentication required");
  return false;
}

Action actionFromBody(JsonDocument& doc, bool& valid) {
  valid = true;
  if (doc["action"].is<const char*>()) {
    const char* a = doc["action"];
    if (strcmp(a, "on") == 0) return Action::On;
    if (strcmp(a, "off") == 0) return Action::Off;
    if (strcmp(a, "toggle") == 0) return Action::Toggle;
    valid = false;
    return Action::Toggle;
  }
  if (doc["state"].is<bool>()) {
    return doc["state"].as<bool>() ? Action::On : Action::Off;
  }
  valid = false;
  return Action::Toggle;
}

// --- route handlers -------------------------------------------------------

void handleInfo(AsyncWebServerRequest* req) {
  JsonDocument doc;
  doc["uuid"] = DeviceInfo::uuid();
  doc["mac"] = DeviceInfo::macAddress();
  doc["name"] = ConfigStore::device().deviceName;
  doc["firmware"] = DeviceInfo::firmwareVersion();
  doc["hardware"] = DeviceInfo::hardwareRevision();
  doc["relayCount"] = board::kChannelCount;
  doc["hostname"] = WiFiService::hostname();
  doc["wifiConnected"] = WiFiService::isConnected();
  doc["apActive"] = WiFiService::isApActive();
  doc["ssid"] = WiFiService::isConnected() ? WiFiService::ssid() : "";
  doc["rssi"] = WiFiService::rssi();
  doc["ip"] = WiFiService::isConnected() ? WiFiService::localIp().toString()
                                         : WiFiService::apIp().toString();
  doc["claimed"] = Security::isClaimed();
  // The claim code is only exposed while the device is unclaimed. Afterwards it
  // is the local-auth password and must not be readable without auth.
  doc["claimCode"] = Security::isClaimed() ? "" : Security::claimCode();
  doc["timezone"] = ConfigStore::device().timezone;
  doc["timeSynced"] = TimeService::isSynced();
  char t[40];
  TimeService::formatIso(t, sizeof(t));
  doc["time"] = t;

  char* buf = static_cast<char*>(malloc(1024));
  if (!buf) return sendError(req, 500, "out of memory");
  serializeJson(doc, buf, 1024);
  sendJson(req, 200, buf);
  free(buf);
}

void handleState(AsyncWebServerRequest* req) {
  char* buf = static_cast<char*>(malloc(kJsonBuf));
  if (!buf) return sendError(req, 500, "out of memory");
  RelayManager::toJson(buf, kJsonBuf);
  sendJson(req, 200, buf);
  free(buf);
}

void handleRelayPost(AsyncWebServerRequest* req) {
  if (!requireAuth(req)) return;

  const String path = req->url();          // /api/relay/<x>
  const int slash = path.lastIndexOf('/');
  const String which = path.substring(slash + 1);

  JsonDocument doc;
  if (deserializeJson(doc, bodyOf(req))) return sendError(req, 400, "invalid json");

  bool valid = false;
  const Action action = actionFromBody(doc, valid);
  if (!valid) return sendError(req, 400, "action must be on|off|toggle");

  if (which == "all") {
    RelayManager::commandAll(action, Source::App);
    return sendOk(req);
  }
  if (which.startsWith("group")) {
    const uint8_t g = static_cast<uint8_t>(doc["group"] | 0);
    if (g == 0) return sendError(req, 400, "group required");
    RelayManager::commandGroup(g, action, Source::App);
    return sendOk(req);
  }

  const int ch = which.toInt();
  if (ch < 0 || ch >= board::kChannelCount) return sendError(req, 404, "no such channel");

  // Optional timer: {"seconds": N} turns on and schedules an auto-off.
  if (doc["seconds"].is<uint32_t>()) {
    const uint32_t secs = doc["seconds"];
    if (!RelayManager::setAutoOff(static_cast<uint8_t>(ch), secs, Source::App)) {
      return sendError(req, 400, "invalid duration");
    }
    return sendOk(req);
  }

  // Optional revision for conflict-safe writes from a synced client.
  if (doc["rev"].is<uint32_t>()) {
    RelayManager::commandWithRev(static_cast<uint8_t>(ch), action, Source::App,
                                 doc["rev"].as<uint32_t>());
  } else {
    RelayManager::command(static_cast<uint8_t>(ch), action, Source::App);
  }
  sendOk(req);
}

void handleConfigGet(AsyncWebServerRequest* req) {
  char* buf = static_cast<char*>(malloc(kJsonBuf));
  if (!buf) return sendError(req, 500, "out of memory");
  ConfigStore::toJson(buf, kJsonBuf, /*redactSecrets=*/true);
  sendJson(req, 200, buf);
  free(buf);
}

void handleConfigPost(AsyncWebServerRequest* req) {
  if (!requireAuth(req)) return;
  char err[96] = {0};
  if (!ConfigStore::applyJson(bodyOf(req), err, sizeof(err))) {
    return sendError(req, 400, err[0] ? err : "invalid configuration");
  }
  ConfigStore::save();
  SwitchScanner::reconfigure();
  TimeService::applyTimezone();
  sendOk(req);
}

void handleWifiScan(AsyncWebServerRequest* req) {
  char* buf = static_cast<char*>(malloc(kJsonBuf));
  if (!buf) return sendError(req, 500, "out of memory");
  WiFiService::scanToJson(buf, kJsonBuf);
  sendJson(req, 200, buf);
  free(buf);
}

void handleWifiSaved(AsyncWebServerRequest* req) {
  char buf[512];
  WiFiService::savedNetworksToJson(buf, sizeof(buf));
  sendJson(req, 200, buf);
}

void handleWifiPost(AsyncWebServerRequest* req) {
  // Provisioning must stay reachable while the recovery AP is up, otherwise a
  // device that lost its network could never be re-adopted.
  if (!WiFiService::isApActive() && !requireAuth(req)) return;

  JsonDocument doc;
  if (deserializeJson(doc, bodyOf(req))) return sendError(req, 400, "invalid json");
  const char* ssid = doc["ssid"];
  const char* pass = doc["password"] | "";
  if (!ssid || !ssid[0]) return sendError(req, 400, "ssid required");
  if (!WiFiService::addNetwork(ssid, pass)) return sendError(req, 400, "could not save network");
  sendOk(req);
}

void handleWifiForget(AsyncWebServerRequest* req) {
  if (!requireAuth(req)) return;
  JsonDocument doc;
  if (deserializeJson(doc, bodyOf(req))) return sendError(req, 400, "invalid json");
  const char* ssid = doc["ssid"];
  if (!ssid || !WiFiService::forgetNetwork(ssid)) return sendError(req, 404, "not found");
  sendOk(req);
}

void handleDiag(AsyncWebServerRequest* req) {
  char* buf = static_cast<char*>(malloc(kJsonBuf));
  if (!buf) return sendError(req, 500, "out of memory");

  JsonDocument doc;
  doc["uuid"] = DeviceInfo::uuid();
  doc["fw"] = DeviceInfo::firmwareVersion();
  doc["uptimeMs"] = DeviceInfo::uptimeMs();
  doc["freeHeap"] = DeviceInfo::freeHeapBytes();
  doc["minFreeHeap"] = DeviceInfo::minFreeHeapBytes();
  doc["maxAlloc"] = DeviceInfo::maxAllocHeapBytes();
  doc["heapFragPct"] = DeviceInfo::heapFragmentationPct();
  doc["cpuMhz"] = DeviceInfo::cpuFreqMhz();
  doc["resetReason"] = DeviceInfo::resetReason();
  doc["brownout"] = DeviceInfo::lastResetWasBrownout();
  doc["wifi"] = WiFiService::isConnected();
  doc["rssi"] = WiFiService::rssi();
  doc["ip"] = WiFiService::localIp().toString();
  // The portal has always had a row for this; it was rendering blank because
  // the field was never sent. Whether the cloud link is up is exactly what you
  // want to know when remote control is not working.
  doc["mqtt"] = MqttService::isEnabled()
                    ? (MqttService::isConnected() ? "connected" : "disconnected")
                    : "disabled";
  doc["mqttReconnects"] = MqttService::reconnectCount();
  // Uncalibrated on WROOM-32 and reads well above ambient. Useful only as a
  // trend line, never as a room temperature.
  doc["chipTempC"] = static_cast<int>(temperatureRead());
  doc["relayRev"] = RelayManager::globalRevision();
  doc["relayMask"] = RelayManager::stateMask();
  doc["wsClients"] = LocalApi::connectedClients();
  doc["nvsFreeEntries"] = NvsStore::freeEntries();
  char t[40];
  TimeService::formatIso(t, sizeof(t));
  doc["time"] = t;
  doc["timeSynced"] = TimeService::isSynced();
  doc["secondsSinceSync"] = TimeService::secondsSinceSync();

  JsonArray sw = doc["switches"].to<JsonArray>();
  for (uint8_t i = 0; i < board::kChannelCount; ++i) sw.add(SwitchScanner::asserted(i));

  size_t n = serializeJson(doc, buf, kJsonBuf - 1600);
  // Append the log ring last so it can be truncated without breaking the JSON.
  if (n > 2 && n + 1600 < kJsonBuf) {
    char* logbuf = static_cast<char*>(malloc(1400));
    if (logbuf) {
      Logger::dumpRing(logbuf, 1400);
      JsonDocument wrap;
      wrap["log"] = logbuf;
      char tail[1600];
      const size_t tn = serializeJson(wrap, tail, sizeof(tail));
      if (tn > 2 && n + tn < kJsonBuf) {
        buf[n - 1] = ',';                       // replace closing brace
        memcpy(buf + n, tail + 1, tn - 1);      // skip the opening brace
        buf[n + tn - 1] = '\0';
      }
      free(logbuf);
    }
  }

  sendJson(req, 200, buf);
  free(buf);
}

/// GET /api/pins - what each relay pin is actually doing, electrically.
void handlePins(AsyncWebServerRequest* req) {
  char* buf = static_cast<char*>(malloc(2048));
  if (!buf) return sendError(req, 500, "out of memory");
  RelayManager::toPinDiagnosticsJson(buf, 2048);
  sendJson(req, 200, buf);
  free(buf);
}

/// POST /api/selftest - walks every channel on then off, one at a time.
///
/// Deliberately sequential and slow. Switching eight coils together on an
/// undersized supply browns the board out, and the reboot looks like a
/// firmware fault; one at a time isolates a genuinely dead channel from a
/// power problem that only appears under load.
void handleSelfTest(AsyncWebServerRequest* req) {
  if (!requireAuth(req)) return;

  JsonDocument doc;
  deserializeJson(doc, bodyOf(req));
  const uint32_t dwellMs = doc["dwellMs"] | 700u;
  if (dwellMs > 5000) return sendError(req, 400, "dwellMs too large");

  // Run off-task: this takes seconds and must not block the HTTP stack.
  static uint32_t s_dwell;
  s_dwell = dwellMs;
  xTaskCreate(
      [](void*) {
        SH_LOGW("test", "self test starting - %u channels",
                static_cast<unsigned>(board::kChannelCount));
        RelayManager::commandAll(Action::Off, Source::App);
        vTaskDelay(pdMS_TO_TICKS(600));

        for (uint8_t ch = 0; ch < board::kChannelCount; ++ch) {
          RelayManager::command(ch, Action::On, Source::App);
          vTaskDelay(pdMS_TO_TICKS(s_dwell));
          const int level = RelayManager::rawPinLevel(ch);
          SH_LOGW("test", "ch%u gpio%d -> ON, pin reads %s",
                  static_cast<unsigned>(ch), board::kRelayPins[ch],
                  level ? "HIGH" : "LOW");
          RelayManager::command(ch, Action::Off, Source::App);
          vTaskDelay(pdMS_TO_TICKS(300));
        }
        SH_LOGW("test", "self test complete");
        vTaskDelete(nullptr);
      },
      "selftest", 3072, nullptr, 1, nullptr);

  sendJson(req, 202, "{\"started\":true}");
}

void handleSchedulesGet(AsyncWebServerRequest* req) {
  char* buf = static_cast<char*>(malloc(kJsonBuf));
  if (!buf) return sendError(req, 500, "out of memory");
  Scheduler::toJson(buf, kJsonBuf);
  sendJson(req, 200, buf);
  free(buf);
}

void handleSchedulesPost(AsyncWebServerRequest* req) {
  if (!requireAuth(req)) return;
  char err[96] = {0};
  if (!Scheduler::applyJson(bodyOf(req), err, sizeof(err))) {
    return sendError(req, 400, err[0] ? err : "invalid schedules");
  }
  Scheduler::save();
  sendOk(req);
}

void handleAutomationsGet(AsyncWebServerRequest* req) {
  char* buf = static_cast<char*>(malloc(kJsonBuf));
  if (!buf) return sendError(req, 500, "out of memory");
  AutomationEngine::toJson(buf, kJsonBuf);
  sendJson(req, 200, buf);
  free(buf);
}

void handleAutomationsPost(AsyncWebServerRequest* req) {
  if (!requireAuth(req)) return;
  char err[96] = {0};
  if (!AutomationEngine::applyJson(bodyOf(req), err, sizeof(err))) {
    return sendError(req, 400, err[0] ? err : "invalid automations");
  }
  AutomationEngine::save();
  sendOk(req);
}

const char* otaStateName(OtaState s) {
  switch (s) {
    case OtaState::Checking:      return "checking";
    case OtaState::Downloading:   return "downloading";
    case OtaState::Verifying:     return "verifying";
    case OtaState::Applying:      return "applying";
    case OtaState::PendingReboot: return "pending_reboot";
    case OtaState::Failed:        return "failed";
    default:                      return "idle";
  }
}

void handleOtaStatus(AsyncWebServerRequest* req) {
  char buf[320];
  JsonDocument doc;
  doc["state"] = otaStateName(OtaService::state());
  doc["progress"] = OtaService::progressPercent();
  doc["error"] = OtaService::lastError();
  doc["partition"] = OtaService::runningPartition();
  doc["pendingVerify"] = OtaService::isPendingVerify();
  doc["version"] = DeviceInfo::firmwareVersion();
  serializeJson(doc, buf, sizeof(buf));
  sendJson(req, 200, buf);
}

void handleOtaCheck(AsyncWebServerRequest* req) {
  if (!requireAuth(req)) return;
  JsonDocument doc;
  deserializeJson(doc, bodyOf(req));  // body is optional

  const char* url = doc["url"];
  bool started = false;
  if (url && url[0]) {
    started = OtaService::updateFrom(url, doc["sha256"] | nullptr);
  } else {
    started = OtaService::checkAndUpdate(doc["force"] | false);
  }
  if (!started) return sendError(req, 409, "update already running or unavailable");
  sendOk(req);
}

void handleLocalAuth(AsyncWebServerRequest* req) {
  JsonDocument doc;
  if (deserializeJson(doc, bodyOf(req))) return sendError(req, 400, "invalid json");
  const char* code = doc["code"];
  if (!code || !Security::secureEquals(code, Security::claimCode())) {
    // Deliberately slow: this is the only brute-forceable secret on the device.
    vTaskDelay(pdMS_TO_TICKS(750));
    return sendError(req, 403, "wrong code");
  }
  Security::randomHex(g_sessionToken, 32);
  g_sessionIssuedMs = millis();

  char cookie[96];
  snprintf(cookie, sizeof(cookie), "sh_session=%s; Path=/; Max-Age=43200; SameSite=Strict",
           g_sessionToken);
  AsyncWebServerResponse* res = req->beginResponse(200, "application/json", "{\"ok\":true}");
  res->addHeader("Set-Cookie", cookie);
  req->send(res);
  SH_LOGI(TAG, "local session issued");
}

void handleClaim(AsyncWebServerRequest* req) {
  JsonDocument doc;
  if (deserializeJson(doc, bodyOf(req))) return sendError(req, 400, "invalid json");

  // Re-claiming a claimed device requires proving you already control it.
  if (Security::isClaimed() && !authorized(req)) {
    return sendError(req, 401, "already claimed");
  }
  const char* code = doc["claimCode"];
  if (!code || !Security::secureEquals(code, Security::claimCode())) {
    vTaskDelay(pdMS_TO_TICKS(750));
    return sendError(req, 403, "wrong claim code");
  }
  const char* key = doc["apiKey"];
  if (!key) return sendError(req, 400, "apiKey required");

  if (!Security::markClaimed(key, doc["homeId"] | "", doc["roomId"] | "")) {
    return sendError(req, 400, "invalid apiKey");
  }
  ConfigStore::setCloud(doc["apiBase"] | "", doc["mqttHost"] | "",
                        doc["mqttPort"] | 8883, doc["mqttTls"] | true,
                        doc["mqttUser"] | "", doc["mqttPass"] | "");

  Event e;
  e.type = EventType::DeviceClaimed;
  EventBus::publish(e);
  sendOk(req);
}

void handleReboot(AsyncWebServerRequest* req) {
  if (!requireAuth(req)) return;
  sendOk(req);
  RelayManager::flush();
  // Give the response time to leave the wire before the stack disappears.
  xTaskCreate([](void*) {
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
  }, "reboot", 2048, nullptr, 1, nullptr);
}

void handleFactoryReset(AsyncWebServerRequest* req) {
  JsonDocument doc;
  if (deserializeJson(doc, bodyOf(req))) return sendError(req, 400, "invalid json");
  const char* confirm = doc["confirm"];
  if (!confirm || !Security::secureEquals(confirm, Security::claimCode())) {
    vTaskDelay(pdMS_TO_TICKS(750));
    return sendError(req, 403, "confirm must be the claim code");
  }
  sendOk(req);
  Event e;
  e.type = EventType::FactoryReset;
  e.source = Source::App;
  EventBus::publish(e);
}

// --- captive portal -------------------------------------------------------

bool isCaptiveProbe(AsyncWebServerRequest* req) {
  const String u = req->url();
  return u.startsWith("/generate_204") || u.startsWith("/gen_204") ||
         u.startsWith("/hotspot-detect") || u.startsWith("/connecttest") ||
         u.startsWith("/ncsi.txt") || u.startsWith("/fwlink") ||
         u.startsWith("/canonical.html") || u.startsWith("/success.txt") ||
         u.startsWith("/redirect");
}

void handleNotFound(AsyncWebServerRequest* req) {
  if (req->method() == HTTP_OPTIONS) {
    req->send(204);
    return;
  }
  // Hue emulation shares port 80. Its paths are /api/<username>/... and never
  // collide with this firmware's own fixed /api/<name> routes, which are
  // matched first, so anything left over that looks like Hue goes to Alexa.
  if (AlexaLocal::handleUnmatched(req, bodyOf(req))) return;

  if (WiFiService::isApActive() && (isCaptiveProbe(req) || !req->url().startsWith("/api"))) {
    // Anything that is not our API gets bounced to the portal, which is what
    // makes phones pop the "Sign in to network" sheet automatically.
    AsyncWebServerResponse* res = req->beginResponse(302);
    char loc[48];
    snprintf(loc, sizeof(loc), "http://%s/", WiFiService::apIp().toString().c_str());
    res->addHeader("Location", loc);
    req->send(res);
    return;
  }
  sendError(req, 404, "not found");
}

// --- websocket ------------------------------------------------------------

void onWsEvent(AsyncWebSocket* /*server*/, AsyncWebSocketClient* client,
               AwsEventType type, void* /*arg*/, uint8_t* data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT: {
      SH_LOGI(TAG, "ws client %u connected", static_cast<unsigned>(client->id()));
      char* buf = static_cast<char*>(malloc(kJsonBuf));
      if (buf) {
        JsonDocument doc;
        doc["type"] = "hello";
        doc["uuid"] = DeviceInfo::uuid();
        doc["fw"] = DeviceInfo::firmwareVersion();
        doc["channels"] = board::kChannelCount;
        const size_t n = serializeJson(doc, buf, kJsonBuf);
        client->text(buf, n);
        free(buf);
      }
      // Nudge the broadcast task to push a snapshot to everyone.
      if (g_events) {
        WsEvent ev{0, false, Source::Unknown, 0, true};
        xQueueSend(g_events, &ev, 0);
      }
      break;
    }
    case WS_EVT_DISCONNECT:
      SH_LOGI(TAG, "ws client disconnected");
      break;
    case WS_EVT_DATA: {
      if (len == 0 || len > 512) return;
      char body[513];
      memcpy(body, data, len);
      body[len] = '\0';

      JsonDocument doc;
      if (deserializeJson(doc, body)) return;

      // Commands over the socket require the same authority as REST. Without
      // a key the socket is read-only on a claimed device.
      if (Security::isClaimed()) {
        const char* key = doc["key"];
        const bool ok =
            (key && Security::secureEquals(key, Security::apiKey())) ||
            (sessionValid() && doc["session"].is<const char*>() &&
             Security::secureEquals(doc["session"], g_sessionToken));
        if (!ok) {
          client->text("{\"type\":\"error\",\"error\":\"unauthorized\"}");
          return;
        }
      }

      const char* cmd = doc["cmd"] | "";
      if (strcmp(cmd, "set") == 0) {
        bool valid = false;
        const Action a = actionFromBody(doc, valid);
        const int ch = doc["channel"] | -1;
        if (valid && ch >= 0 && ch < board::kChannelCount) {
          RelayManager::command(static_cast<uint8_t>(ch), a, Source::App);
        }
      } else if (strcmp(cmd, "all") == 0) {
        bool valid = false;
        const Action a = actionFromBody(doc, valid);
        if (valid) RelayManager::commandAll(a, Source::App);
      }
      break;
    }
    default:
      break;
  }
}

// --- event bus bridge -----------------------------------------------------

void onBusEvent(const Event& e, void* /*ctx*/) {
  if (!g_events) return;
  WsEvent ev{};
  switch (e.type) {
    case EventType::RelayChanged:
      ev = {e.channel, e.state, e.source, e.rev, false};
      break;
    case EventType::ConfigChanged:
    case EventType::WifiConnected:
    case EventType::TimeSynced:
      ev = {0, false, Source::Unknown, 0, true};
      break;
    default:
      return;
  }
  // Non-blocking by contract: dropping a UI update under pressure is fine,
  // stalling the relay task is not.
  xQueueSend(g_events, &ev, 0);
}

}  // namespace

// ---------------------------------------------------------------------------

void LocalApi::begin() {
  if (!ConfigStore::device().localApiEnabled) {
    SH_LOGW(TAG, "local API disabled by configuration");
    return;
  }

  g_events = xQueueCreate(24, sizeof(WsEvent));

  g_ws.onEvent(onWsEvent);
  g_server.addHandler(&g_ws);

  g_server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    // Pointer + length overload, NOT the const char* one. The char* overload
    // copies the whole ~20 KB page into a heap String first; at ~50% heap
    // fragmentation that allocation intermittently fails and the response
    // goes out EMPTY - a blank page from a healthy device, at random, more
    // often as the page grows. This overload streams from flash, no copy.
    AsyncWebServerResponse* res = req->beginResponse(
        200, "text/html; charset=utf-8",
        reinterpret_cast<const uint8_t*>(kPortalHtml), sizeof(kPortalHtml) - 1);
    // Deliberately not cached. This page is ~15 KB served from flash over a
    // LAN, so caching saves nothing worth having - and it guarantees that
    // after a firmware update the browser keeps running the OLD page. That
    // failure is invisible and maddening: the device is healthy, the page
    // renders, and the stale JavaScript underneath it misbehaves.
    res->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    req->send(res);
  });

  g_server.on("/api/info", HTTP_GET, handleInfo);
  g_server.on("/api/state", HTTP_GET, handleState);
  // NOT /api/config - that path belongs to the Hue protocol, which Alexa uses
  // to identify a bridge. Serving our own configuration there made every Alexa
  // discovery fail. See AlexaLocal::attach.
  g_server.on("/api/settings", HTTP_GET, handleConfigGet);
  g_server.on("/api/diag", HTTP_GET, handleDiag);
  g_server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  g_server.on("/api/wifi/saved", HTTP_GET, handleWifiSaved);
  g_server.on("/api/schedules", HTTP_GET, handleSchedulesGet);
  g_server.on("/api/automations", HTTP_GET, handleAutomationsGet);
  g_server.on("/api/ota/status", HTTP_GET, handleOtaStatus);
  g_server.on("/api/pins", HTTP_GET, handlePins);
  g_server.on("/api/selftest", HTTP_POST, handleSelfTest, nullptr, collectBody);

  g_server.on("/api/schedules", HTTP_POST, handleSchedulesPost, nullptr, collectBody);
  g_server.on("/api/automations", HTTP_POST, handleAutomationsPost, nullptr, collectBody);
  g_server.on("/api/ota/check", HTTP_POST, handleOtaCheck, nullptr, collectBody);

  g_server.on("/api/settings", HTTP_POST, handleConfigPost, nullptr, collectBody);
  g_server.on("/api/wifi", HTTP_POST, handleWifiPost, nullptr, collectBody);
  g_server.on("/api/wifi/forget", HTTP_POST, handleWifiForget, nullptr, collectBody);
  g_server.on("/api/local-auth", HTTP_POST, handleLocalAuth, nullptr, collectBody);
  g_server.on("/api/claim", HTTP_POST, handleClaim, nullptr, collectBody);
  g_server.on("/api/reboot", HTTP_POST, handleReboot, nullptr, collectBody);
  g_server.on("/api/factory-reset", HTTP_POST, handleFactoryReset, nullptr, collectBody);

  // Relay routes are matched by prefix so /api/relay/<n>, /api/relay/all and
  // /api/relay/group all land in one handler.
  g_server.on("^\\/api\\/relay\\/[a-z0-9]+$", HTTP_POST, handleRelayPost, nullptr,
              collectBody);

  // Hue bridge emulation lives on the same server (Echo insists on port 80).
  AlexaLocal::attach(g_server);

  g_server.onNotFound(handleNotFound);
  // Catch-all body collector: unmatched requests (which is how Hue's
  // PUT /api/<user>/lights/<n>/state arrives) still get their body.
  g_server.onRequestBody(collectBody);

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers",
                                       "Content-Type, X-API-Key");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods",
                                       "GET, POST, OPTIONS");

  g_server.begin();
  g_running = true;

  EventBus::subscribe(onBusEvent, nullptr, "localapi");
  SH_LOGI(TAG, "local API listening on :%u", static_cast<unsigned>(kPort));
}

void LocalApi::startTask() {
  if (!g_running || g_task) return;
  xTaskCreatePinnedToCore(&LocalApi::taskEntry, "httpsvc", 6144, nullptr, 2,
                          &g_task, 0);
}

void LocalApi::taskEntry(void* /*arg*/) {
  esp_task_wdt_add(nullptr);
  bool dnsWasUp = false;

  for (;;) {
    esp_task_wdt_reset();

    // Captive-portal DNS runs only while the AP is up.
    const bool apUp = WiFiService::isApActive();
    if (apUp && !dnsWasUp) {
      g_dns.setErrorReplyCode(DNSReplyCode::NoError);
      g_dns.start(53, "*", WiFiService::apIp());
      dnsWasUp = true;
      g_dnsUp = true;
      SH_LOGI(TAG, "captive DNS started on %s",
              WiFiService::apIp().toString().c_str());
    } else if (!apUp && dnsWasUp) {
      g_dns.stop();
      dnsWasUp = false;
      g_dnsUp = false;
    }
    if (dnsWasUp) g_dns.processNextRequest();

    WsEvent ev{};
    while (xQueueReceive(g_events, &ev, 0) == pdTRUE) {
      if (g_ws.count() == 0) continue;
      if (ev.snapshot) {
        broadcastSnapshot();
      } else {
        char buf[224];
        JsonDocument doc;
        doc["type"] = "relay";
        doc["channel"] = ev.channel;
        doc["state"] = ev.state;
        doc["source"] = toString(ev.source);
        doc["rev"] = ev.rev;
        const size_t n = serializeJson(doc, buf, sizeof(buf));
        g_ws.textAll(buf, n);
      }
    }

    g_ws.cleanupClients();
    vTaskDelay(pdMS_TO_TICKS(dnsWasUp ? 10 : 40));
  }
}

void LocalApi::broadcastSnapshot() {
  if (g_ws.count() == 0) return;
  char* buf = static_cast<char*>(malloc(kJsonBuf));
  if (!buf) return;
  // {"type":"state","data":<snapshot>}
  const char* prefix = "{\"type\":\"state\",\"data\":";
  const size_t plen = strlen(prefix);
  memcpy(buf, prefix, plen);
  const size_t n = RelayManager::toJson(buf + plen, kJsonBuf - plen - 2);
  buf[plen + n] = '}';
  buf[plen + n + 1] = '\0';
  g_ws.textAll(buf, plen + n + 1);
  free(buf);
}

bool LocalApi::isRunning() { return g_running; }
uint32_t LocalApi::connectedClients() { return g_ws.count(); }

}  // namespace sh
