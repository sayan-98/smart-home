#include "alexa/AlexaLocal.h"

#include <ArduinoJson.h>
#include <Arduino.h>
#include <AsyncUDP.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "config/Board.h"
#include "config/ConfigStore.h"
#include "device/DeviceInfo.h"
#include "log/Logger.h"
#include "net/WiFiService.h"
#include "relay/RelayManager.h"

namespace sh {
namespace {

constexpr const char* TAG = "alexa";
constexpr uint16_t kSsdpPort = 1900;

AsyncUDP g_udp;
bool     g_udpUp = false;
uint32_t g_discoveries = 0;

char g_bridgeId[17] = {0};   // AABBCCFFFEDDEEFF
char g_macFlat[13] = {0};    // aabbccddeeff
char g_udn[48] = {0};        // uuid:2f402f80-da50-11e1-9b23-aabbccddeeff

/// Hue exposes lights as 1-based ids; channels are 0-based.
inline uint8_t idToChannel(int id) { return static_cast<uint8_t>(id - 1); }
inline int channelToId(uint8_t ch) { return ch + 1; }

void buildIdentity() {
  const char* mac = DeviceInfo::macAddress();  // "AA:BB:CC:DD:EE:FF"
  uint8_t b[6] = {0};
  sscanf(mac, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &b[0], &b[1], &b[2], &b[3],
         &b[4], &b[5]);
  snprintf(g_bridgeId, sizeof(g_bridgeId), "%02X%02X%02XFFFE%02X%02X%02X", b[0],
           b[1], b[2], b[3], b[4], b[5]);
  snprintf(g_macFlat, sizeof(g_macFlat), "%02x%02x%02x%02x%02x%02x", b[0], b[1],
           b[2], b[3], b[4], b[5]);
  // The prefix is the well-known Hue bridge UUID namespace; Echo matches on it.
  snprintf(g_udn, sizeof(g_udn), "uuid:2f402f80-da50-11e1-9b23-%s", g_macFlat);
}

void sendSsdpResponse(const IPAddress& to, uint16_t port, const char* st) {
  const IPAddress ip = WiFi.localIP();
  char msg[420];
  const int n = snprintf(
      msg, sizeof(msg),
      "HTTP/1.1 200 OK\r\n"
      "EXT:\r\n"
      "CACHE-CONTROL: max-age=100\r\n"
      "LOCATION: http://%u.%u.%u.%u:80/description.xml\r\n"
      "SERVER: FreeRTOS/10.4, UPnP/1.0, IpBridge/1.24.0\r\n"
      "hue-bridgeid: %s\r\n"
      "ST: %s\r\n"
      "USN: %s::%s\r\n"
      "\r\n",
      ip[0], ip[1], ip[2], ip[3], g_bridgeId, st, g_udn, st);
  if (n <= 0) return;
  g_udp.writeTo(reinterpret_cast<const uint8_t*>(msg), static_cast<size_t>(n), to,
                port);
}

void onSsdpPacket(AsyncUDPPacket& packet) {
  const size_t len = packet.length();
  if (len < 16 || len > 1024) return;

  // Copy so we can NUL-terminate and lowercase for matching.
  char buf[1025];
  memcpy(buf, packet.data(), len);
  buf[len] = '\0';
  if (strncmp(buf, "M-SEARCH", 8) != 0) return;

  for (size_t i = 0; i < len; ++i) buf[i] = static_cast<char>(tolower(buf[i]));

  // Answer the search targets a Hue bridge is expected to answer.
  const bool wantsAll = strstr(buf, "ssdp:all") != nullptr;
  const bool wantsRoot = strstr(buf, "upnp:rootdevice") != nullptr;
  const bool wantsBasic = strstr(buf, "urn:schemas-upnp-org:device:basic:1") != nullptr;
  const bool wantsHue = strstr(buf, "libhue:idl") != nullptr;
  if (!wantsAll && !wantsRoot && !wantsBasic && !wantsHue) return;

  g_discoveries++;
  SH_LOGI(TAG, "SSDP M-SEARCH from %s (discovery #%lu)",
          packet.remoteIP().toString().c_str(),
          static_cast<unsigned long>(g_discoveries));

  // Real bridges answer each relevant ST separately; Echo is happier when it
  // sees all three. Small gaps avoid the replies being coalesced and dropped.
  sendSsdpResponse(packet.remoteIP(), packet.remotePort(), "upnp:rootdevice");
  delay(8);
  sendSsdpResponse(packet.remoteIP(), packet.remotePort(), g_udn);
  delay(8);
  sendSsdpResponse(packet.remoteIP(), packet.remotePort(),
                   "urn:schemas-upnp-org:device:basic:1");
}

/// One light object, in Hue v1 shape.
void lightToJson(uint8_t ch, JsonObject o) {
  const ChannelConfig& cfg = ConfigStore::channel(ch);
  const bool on = RelayManager::isOn(ch);

  JsonObject st = o["state"].to<JsonObject>();
  st["on"] = on;
  st["bri"] = on ? 254 : 0;
  st["alert"] = "none";
  st["mode"] = "homeautomation";
  st["reachable"] = true;

  // A socket is an on/off plug, and saying so gets Alexa to expose it as a
  // plug rather than a dimmable light - so "set it to 50%" is rejected
  // honestly instead of silently doing nothing.
  if (ConfigStore::device().alexaAsPlug) {
    o["type"] = "On/Off plug-in unit";
    o["modelid"] = "LOM001";
    o["productname"] = "On/Off plug";
  } else {
    // Fallback for Echo firmware that only discovers lights.
    o["type"] = "Dimmable light";
    o["modelid"] = "LWB010";
    o["productname"] = "Hue white lamp";
  }
  o["name"] = cfg.name;
  o["manufacturername"] = "Signify Netherlands B.V.";
  o["swversion"] = DeviceInfo::firmwareVersion();

  char uid[40];
  snprintf(uid, sizeof(uid), "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c:00:11-%02x",
           g_macFlat[0], g_macFlat[1], g_macFlat[2], g_macFlat[3], g_macFlat[4],
           g_macFlat[5], g_macFlat[6], g_macFlat[7], g_macFlat[8], g_macFlat[9],
           g_macFlat[10], g_macFlat[11], channelToId(ch));
  o["uniqueid"] = uid;
}

size_t lightsToJson(char* out, size_t cap) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  for (uint8_t ch = 0; ch < board::kChannelCount; ++ch) {
    if (!ConfigStore::channel(ch).relayEnabled) continue;
    char key[4];
    snprintf(key, sizeof(key), "%d", channelToId(ch));
    lightToJson(ch, root[key].to<JsonObject>());
  }
  return serializeJson(doc, out, cap);
}

void sendJson(AsyncWebServerRequest* req, const char* json) {
  AsyncWebServerResponse* res = req->beginResponse(200, "application/json", json);
  res->addHeader("Cache-Control", "no-store");
  req->send(res);
}

/// Splits "/api/<user>/lights/3/state" into its parts.
struct HuePath {
  bool     valid = false;
  bool     hasUser = false;
  bool     isLights = false;
  bool     isState = false;
  bool     isConfig = false;
  int      lightId = -1;
};

HuePath parsePath(const String& url) {
  HuePath p;
  if (!url.startsWith("/api")) return p;

  // Tokenise after "/api".
  String rest = url.substring(4);
  if (rest.startsWith("/")) rest = rest.substring(1);
  if (rest.length() == 0) return p;  // bare /api, handled separately

  int slash = rest.indexOf('/');
  const String user = slash < 0 ? rest : rest.substring(0, slash);
  if (user.length() == 0) return p;
  p.hasUser = true;
  p.valid = true;
  if (slash < 0) return p;  // /api/<user>

  String tail = rest.substring(slash + 1);
  if (tail == "config" || tail.startsWith("config/")) {
    p.isConfig = true;
    return p;
  }
  if (!tail.startsWith("lights")) return p;
  p.isLights = true;

  tail = tail.substring(6);
  if (tail.startsWith("/")) tail = tail.substring(1);
  if (tail.length() == 0) return p;  // /api/<user>/lights

  slash = tail.indexOf('/');
  const String idStr = slash < 0 ? tail : tail.substring(0, slash);
  p.lightId = idStr.toInt();
  if (slash >= 0 && tail.substring(slash + 1).startsWith("state")) p.isState = true;
  return p;
}

}  // namespace

// ---------------------------------------------------------------------------

void AlexaLocal::begin() {
  buildIdentity();
  SH_LOGI(TAG, "hue bridge id %s, udn %s", g_bridgeId, g_udn);
}

const char* AlexaLocal::bridgeId() { return g_bridgeId; }
bool AlexaLocal::isEnabled() { return ConfigStore::device().alexaEnabled; }
uint32_t AlexaLocal::discoveryRequests() { return g_discoveries; }

void AlexaLocal::onNetworkUp() {
  if (!isEnabled()) return;
  if (g_udpUp) return;
  if (!g_udp.listenMulticast(IPAddress(239, 255, 255, 250), kSsdpPort)) {
    SH_LOGE(TAG, "SSDP multicast listen failed - Echo will not discover this device");
    return;
  }
  g_udp.onPacket(onSsdpPacket);
  g_udpUp = true;
  SH_LOGI(TAG, "SSDP responder up. Say: \"Alexa, discover devices\"");
}

void AlexaLocal::onNetworkDown() {
  if (!g_udpUp) return;
  g_udp.close();
  g_udpUp = false;
}

void AlexaLocal::attach(AsyncWebServer& server) {
  // The UPnP description Echo fetches after SSDP.
  server.on("/description.xml", HTTP_GET, [](AsyncWebServerRequest* req) {
    const IPAddress ip = WiFi.localIP();
    char* xml = static_cast<char*>(malloc(1024));
    if (!xml) return req->send(500);
    snprintf(xml, 1024,
             "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
             "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
             "<specVersion><major>1</major><minor>0</minor></specVersion>"
             "<URLBase>http://%u.%u.%u.%u:80/</URLBase>"
             "<device>"
             "<deviceType>urn:schemas-upnp-org:device:Basic:1</deviceType>"
             "<friendlyName>%s (%u.%u.%u.%u)</friendlyName>"
             "<manufacturer>Royal Philips Electronics</manufacturer>"
             "<manufacturerURL>http://www.philips.com</manufacturerURL>"
             "<modelDescription>Philips hue Personal Wireless Lighting</modelDescription>"
             "<modelName>Philips hue bridge 2015</modelName>"
             "<modelNumber>BSB002</modelNumber>"
             "<modelURL>http://www.meethue.com</modelURL>"
             "<serialNumber>%s</serialNumber>"
             "<UDN>%s</UDN>"
             "<presentationURL>index.html</presentationURL>"
             "</device></root>",
             ip[0], ip[1], ip[2], ip[3], ConfigStore::device().deviceName, ip[0],
             ip[1], ip[2], ip[3], g_macFlat, g_udn);
    AsyncWebServerResponse* res = req->beginResponse(200, "application/xml", xml);
    req->send(res);
    free(xml);
    SH_LOGI(TAG, "description.xml served to %s", req->client()->remoteIP().toString().c_str());
  });

  // The "link button" pairing call. A real bridge requires a physical button
  // press; emulators always accept, which is what makes discovery one-step.
  server.on("/api", HTTP_POST, [](AsyncWebServerRequest* req) {
    char body[128];
    snprintf(body, sizeof(body),
             "[{\"success\":{\"username\":\"%s\"}}]", g_macFlat);
    sendJson(req, body);
    SH_LOGI(TAG, "hue pairing accepted from %s",
            req->client()->remoteIP().toString().c_str());
  });
}

bool AlexaLocal::handleUnmatched(AsyncWebServerRequest* req, const char* body) {
  if (!isEnabled()) return false;

  const String url = req->url();
  if (!url.startsWith("/api/")) return false;

  const HuePath p = parsePath(url);
  if (!p.valid || !p.hasUser) return false;

  // ---- PUT /api/<user>/lights/<id>/state : the actual command from Alexa ---
  if (p.isState && p.lightId >= 1 && p.lightId <= board::kChannelCount &&
      req->method() == HTTP_PUT) {
    JsonDocument doc;
    if (deserializeJson(doc, body ? body : "")) {
      req->send(400, "application/json", "[{\"error\":{\"description\":\"bad json\"}}]");
      return true;
    }
    const uint8_t ch = idToChannel(p.lightId);

    bool on = RelayManager::isOn(ch);
    if (doc["on"].is<bool>()) {
      on = doc["on"].as<bool>();
    } else if (doc["bri"].is<int>()) {
      // Some skills send brightness alone. Treat any non-zero level as ON so
      // "Alexa, set X to 40%" at least does the sensible thing.
      on = doc["bri"].as<int>() > 0;
    }

    RelayManager::command(ch, on ? Action::On : Action::Off, Source::Alexa);
    SH_LOGI(TAG, "voice: %s -> %s", ConfigStore::channel(ch).name, on ? "ON" : "OFF");

    char resp[224];
    snprintf(resp, sizeof(resp),
             "[{\"success\":{\"/lights/%d/state/on\":%s}},"
             "{\"success\":{\"/lights/%d/state/bri\":%d}}]",
             p.lightId, on ? "true" : "false", p.lightId, on ? 254 : 0);
    sendJson(req, resp);
    return true;
  }

  // ---- GET /api/<user>/lights/<id> ---------------------------------------
  if (p.isLights && p.lightId >= 1 && p.lightId <= board::kChannelCount) {
    JsonDocument doc;
    lightToJson(idToChannel(p.lightId), doc.to<JsonObject>());
    char buf[512];
    serializeJson(doc, buf, sizeof(buf));
    sendJson(req, buf);
    return true;
  }

  // ---- GET /api/<user>/lights --------------------------------------------
  if (p.isLights) {
    char* buf = static_cast<char*>(malloc(3072));
    if (!buf) {
      req->send(500);
      return true;
    }
    lightsToJson(buf, 3072);
    sendJson(req, buf);
    free(buf);
    SH_LOGI(TAG, "light list served (%u channels)",
            static_cast<unsigned>(board::kChannelCount));
    return true;
  }

  // ---- GET /api/<user>/config and GET /api/<user> -------------------------
  {
    const IPAddress ip = WiFi.localIP();
    JsonDocument doc;
    JsonObject cfg = p.isConfig ? doc.to<JsonObject>() : doc["config"].to<JsonObject>();
    cfg["name"] = "Philips hue";
    cfg["bridgeid"] = g_bridgeId;
    cfg["modelid"] = "BSB002";
    cfg["swversion"] = "1948086000";
    cfg["apiversion"] = "1.24.0";
    cfg["mac"] = DeviceInfo::macAddress();
    cfg["ipaddress"] = ip.toString();
    cfg["netmask"] = WiFi.subnetMask().toString();
    cfg["gateway"] = WiFi.gatewayIP().toString();
    cfg["dhcp"] = true;
    cfg["linkbutton"] = false;
    cfg["factorynew"] = false;
    JsonObject whitelist = cfg["whitelist"].to<JsonObject>();
    JsonObject entry = whitelist[g_macFlat].to<JsonObject>();
    entry["name"] = "SmartHomeOS";
    entry["create date"] = "2026-01-01T00:00:00";
    entry["last use date"] = "2026-01-01T00:00:00";

    if (!p.isConfig) {
      JsonObject lights = doc["lights"].to<JsonObject>();
      for (uint8_t ch = 0; ch < board::kChannelCount; ++ch) {
        if (!ConfigStore::channel(ch).relayEnabled) continue;
        char key[4];
        snprintf(key, sizeof(key), "%d", channelToId(ch));
        lightToJson(ch, lights[key].to<JsonObject>());
      }
      doc["groups"].to<JsonObject>();
      doc["scenes"].to<JsonObject>();
      doc["schedules"].to<JsonObject>();
    }

    char* buf = static_cast<char*>(malloc(4096));
    if (!buf) {
      req->send(500);
      return true;
    }
    serializeJson(doc, buf, 4096);
    sendJson(req, buf);
    free(buf);
    return true;
  }
}

}  // namespace sh
