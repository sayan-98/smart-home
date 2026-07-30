#include "config/ConfigStore.h"

#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <string.h>

#include "core/EventBus.h"
#include "log/Logger.h"
#include "storage/NvsStore.h"

namespace sh {
namespace {

constexpr const char* TAG = "cfg";
constexpr const char* kKey = "json";
constexpr size_t kJsonCap = 4000;

DeviceConfig g_device{};
ChannelConfig g_channels[board::kChannelCount]{};
SemaphoreHandle_t g_mutex = nullptr;

void copyStr(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

const char* restoreName(RestoreMode m) {
  switch (m) {
    case RestoreMode::On:   return "on";
    case RestoreMode::Last: return "last";
    default:                return "off";
  }
}

RestoreMode restoreFrom(const char* s, RestoreMode fallback) {
  if (!s) return fallback;
  if (strcmp(s, "on") == 0) return RestoreMode::On;
  if (strcmp(s, "last") == 0) return RestoreMode::Last;
  if (strcmp(s, "off") == 0) return RestoreMode::Off;
  return fallback;
}

const char* switchModeName(SwitchMode m) {
  return m == SwitchMode::Latching ? "latching" : "momentary";
}

SwitchMode switchModeFrom(const char* s, SwitchMode fallback) {
  if (!s) return fallback;
  if (strcmp(s, "latching") == 0) return SwitchMode::Latching;
  if (strcmp(s, "momentary") == 0) return SwitchMode::Momentary;
  return fallback;
}

const char* gestureName(GestureAction a) {
  switch (a) {
    case GestureAction::ToggleOwn:   return "toggle";
    case GestureAction::AllOff:      return "all_off";
    case GestureAction::AllOn:       return "all_on";
    case GestureAction::GroupToggle: return "group_toggle";
    default:                         return "none";
  }
}

GestureAction gestureFrom(const char* s, GestureAction fallback) {
  if (!s) return fallback;
  if (strcmp(s, "toggle") == 0) return GestureAction::ToggleOwn;
  if (strcmp(s, "all_off") == 0) return GestureAction::AllOff;
  if (strcmp(s, "all_on") == 0) return GestureAction::AllOn;
  if (strcmp(s, "group_toggle") == 0) return GestureAction::GroupToggle;
  if (strcmp(s, "none") == 0) return GestureAction::None;
  return fallback;
}

struct Lock {
  bool taken = false;
  Lock() {
    if (g_mutex) taken = xSemaphoreTake(g_mutex, pdMS_TO_TICKS(500)) == pdTRUE;
  }
  ~Lock() {
    if (taken) xSemaphoreGive(g_mutex);
  }
};

}  // namespace

void ConfigStore::loadDefaults() {
  memset(&g_device, 0, sizeof(g_device));
  g_device.version = kSchemaVersion;
  copyStr(g_device.deviceName, sizeof(g_device.deviceName), "Smart Home Node");
  // India. Change via the app or the captive portal. POSIX TZ has the sign
  // inverted relative to UTC offset - "IST-5:30" really means UTC+5:30.
  copyStr(g_device.timezone, sizeof(g_device.timezone), "IST-5:30");
  copyStr(g_device.ntpServer, sizeof(g_device.ntpServer), "pool.ntp.org");
  g_device.mqttPort = 8883;
  g_device.mqttTls = true;
  g_device.alexaEnabled = true;
  g_device.alexaAsPlug = true;
  g_device.localApiEnabled = true;
  g_device.cloudEnabled = false;  // stays off until the device is claimed
  g_device.logLevel = static_cast<uint8_t>(LogLevel::Info);

  for (uint8_t i = 0; i < board::kChannelCount; ++i) {
    ChannelConfig& c = g_channels[i];
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "Socket %u", static_cast<unsigned>(i + 1));
    copyStr(c.icon, sizeof(c.icon), "socket");
    c.relayEnabled = true;
    // OFF is the safe default for mains sockets: a device that comes back from
    // a power cut should not silently re-energise an appliance.
    c.restore = RestoreMode::Off;
    c.switchEnabled = true;
    // Indian modular wall switches stay in position -> latching.
    c.switchMode = SwitchMode::Latching;
    c.switchInverted = false;
    c.debounceMs = board::kDefaultDebounceMs;
    c.longPressMs = board::kDefaultLongPressMs;
    c.doublePressMs = board::kDefaultDoublePressMs;
    c.longPressAction = GestureAction::AllOff;
    c.doublePressAction = GestureAction::GroupToggle;
    c.group = 0;
    c.autoOffSec = 0;
  }
}

void ConfigStore::begin() {
  if (!g_mutex) g_mutex = xSemaphoreCreateMutex();
  loadDefaults();

  char* buf = static_cast<char*>(malloc(kJsonCap));
  if (!buf) {
    SH_LOGE(TAG, "no heap for config load, using defaults");
    return;
  }
  const size_t n = NvsStore::getString(nvsns::kConfig, kKey, buf, kJsonCap, "");
  if (n == 0) {
    SH_LOGW(TAG, "no stored config, defaults applied");
    free(buf);
    save();
    return;
  }

  char err[96] = {0};
  if (!applyJson(buf, err, sizeof(err))) {
    SH_LOGE(TAG, "stored config rejected (%s), defaults applied", err);
    loadDefaults();
  } else {
    SH_LOGI(TAG, "config loaded (v%u, %u channels)",
            static_cast<unsigned>(g_device.version),
            static_cast<unsigned>(board::kChannelCount));
  }
  free(buf);
}

const DeviceConfig& ConfigStore::device() { return g_device; }

const ChannelConfig& ConfigStore::channel(uint8_t index) {
  if (index >= board::kChannelCount) index = 0;
  return g_channels[index];
}

bool ConfigStore::applyJson(const char* json, char* err, size_t errCap) {
  if (err && errCap) err[0] = '\0';
  if (!json || !json[0]) {
    if (err) snprintf(err, errCap, "empty body");
    return false;
  }

  JsonDocument doc;
  const DeserializationError e = deserializeJson(doc, json);
  if (e) {
    if (err) snprintf(err, errCap, "bad json: %s", e.c_str());
    return false;
  }

  // Validate everything BEFORE mutating, so a rejected patch leaves the
  // running configuration untouched.
  JsonArrayConst chans = doc["channels"].as<JsonArrayConst>();
  if (!chans.isNull()) {
    uint8_t idx = 0;
    for (JsonObjectConst c : chans) {
      if (idx >= board::kChannelCount) break;
      const uint32_t dbg = c["debounceMs"] | board::kDefaultDebounceMs;
      if (dbg > 2000) {
        if (err) snprintf(err, errCap, "channel %u debounceMs out of range", idx);
        return false;
      }
      const uint32_t autoOff = c["autoOffSec"] | 0u;
      if (autoOff > 86400u * 7u) {
        if (err) snprintf(err, errCap, "channel %u autoOffSec too large", idx);
        return false;
      }
      idx++;
    }
  }
  const uint32_t port = doc["device"]["mqttPort"] | g_device.mqttPort;
  if (port == 0 || port > 65535) {
    if (err) snprintf(err, errCap, "mqttPort out of range");
    return false;
  }

  Lock lock;

  JsonObjectConst d = doc["device"].as<JsonObjectConst>();
  if (!d.isNull()) {
    g_device.version = d["version"] | kSchemaVersion;
    if (d["name"].is<const char*>())
      copyStr(g_device.deviceName, sizeof(g_device.deviceName), d["name"]);
    if (d["homeId"].is<const char*>())
      copyStr(g_device.homeId, sizeof(g_device.homeId), d["homeId"]);
    if (d["roomId"].is<const char*>())
      copyStr(g_device.roomId, sizeof(g_device.roomId), d["roomId"]);
    if (d["timezone"].is<const char*>())
      copyStr(g_device.timezone, sizeof(g_device.timezone), d["timezone"]);
    if (d["ntpServer"].is<const char*>())
      copyStr(g_device.ntpServer, sizeof(g_device.ntpServer), d["ntpServer"]);
    if (d["mqttHost"].is<const char*>())
      copyStr(g_device.mqttHost, sizeof(g_device.mqttHost), d["mqttHost"]);
    g_device.mqttPort = static_cast<uint16_t>(port);
    g_device.mqttTls = d["mqttTls"] | g_device.mqttTls;
    if (d["mqttUser"].is<const char*>())
      copyStr(g_device.mqttUser, sizeof(g_device.mqttUser), d["mqttUser"]);
    // A redacted password must never overwrite the real one.
    if (d["mqttPass"].is<const char*>()) {
      const char* p = d["mqttPass"];
      if (strcmp(p, "********") != 0)
        copyStr(g_device.mqttPass, sizeof(g_device.mqttPass), p);
    }
    if (d["apiBase"].is<const char*>())
      copyStr(g_device.apiBase, sizeof(g_device.apiBase), d["apiBase"]);
    g_device.alexaEnabled = d["alexaEnabled"] | g_device.alexaEnabled;
    g_device.alexaAsPlug = d["alexaAsPlug"] | g_device.alexaAsPlug;
    g_device.localApiEnabled = d["localApiEnabled"] | g_device.localApiEnabled;
    g_device.cloudEnabled = d["cloudEnabled"] | g_device.cloudEnabled;
    g_device.logLevel = d["logLevel"] | g_device.logLevel;
  }

  if (!chans.isNull()) {
    uint8_t idx = 0;
    for (JsonObjectConst c : chans) {
      if (idx >= board::kChannelCount) break;
      ChannelConfig& t = g_channels[idx];
      if (c["name"].is<const char*>()) copyStr(t.name, sizeof(t.name), c["name"]);
      if (c["icon"].is<const char*>()) copyStr(t.icon, sizeof(t.icon), c["icon"]);
      t.relayEnabled = c["enabled"] | t.relayEnabled;
      t.restore = restoreFrom(c["restore"], t.restore);
      t.switchEnabled = c["switchEnabled"] | t.switchEnabled;
      t.switchMode = switchModeFrom(c["switchMode"], t.switchMode);
      t.switchInverted = c["switchInverted"] | t.switchInverted;
      t.debounceMs = c["debounceMs"] | t.debounceMs;
      t.longPressMs = c["longPressMs"] | t.longPressMs;
      t.doublePressMs = c["doublePressMs"] | t.doublePressMs;
      t.longPressAction = gestureFrom(c["longPressAction"], t.longPressAction);
      t.doublePressAction = gestureFrom(c["doublePressAction"], t.doublePressAction);
      t.group = c["group"] | t.group;
      t.autoOffSec = c["autoOffSec"] | t.autoOffSec;
      idx++;
    }
  }

  return true;
}

size_t ConfigStore::toJson(char* out, size_t cap, bool redactSecrets) {
  JsonDocument doc;
  JsonObject d = doc["device"].to<JsonObject>();
  d["version"] = g_device.version;
  d["name"] = g_device.deviceName;
  d["homeId"] = g_device.homeId;
  d["roomId"] = g_device.roomId;
  d["timezone"] = g_device.timezone;
  d["ntpServer"] = g_device.ntpServer;
  d["mqttHost"] = g_device.mqttHost;
  d["mqttPort"] = g_device.mqttPort;
  d["mqttTls"] = g_device.mqttTls;
  d["mqttUser"] = g_device.mqttUser;
  d["mqttPass"] = redactSecrets ? "********" : g_device.mqttPass;
  d["apiBase"] = g_device.apiBase;
  d["alexaEnabled"] = g_device.alexaEnabled;
  d["alexaAsPlug"] = g_device.alexaAsPlug;
  d["localApiEnabled"] = g_device.localApiEnabled;
  d["cloudEnabled"] = g_device.cloudEnabled;
  d["logLevel"] = g_device.logLevel;

  JsonArray arr = doc["channels"].to<JsonArray>();
  for (uint8_t i = 0; i < board::kChannelCount; ++i) {
    const ChannelConfig& c = g_channels[i];
    JsonObject o = arr.add<JsonObject>();
    o["index"] = i;
    o["name"] = c.name;
    o["icon"] = c.icon;
    o["enabled"] = c.relayEnabled;
    o["restore"] = restoreName(c.restore);
    o["switchEnabled"] = c.switchEnabled;
    o["switchMode"] = switchModeName(c.switchMode);
    o["switchInverted"] = c.switchInverted;
    o["debounceMs"] = c.debounceMs;
    o["longPressMs"] = c.longPressMs;
    o["doublePressMs"] = c.doublePressMs;
    o["longPressAction"] = gestureName(c.longPressAction);
    o["doublePressAction"] = gestureName(c.doublePressAction);
    o["group"] = c.group;
    o["autoOffSec"] = c.autoOffSec;
  }

  return serializeJson(doc, out, cap);
}

bool ConfigStore::save() {
  char* buf = static_cast<char*>(malloc(kJsonCap));
  if (!buf) {
    SH_LOGE(TAG, "no heap to save config");
    return false;
  }
  const size_t n = toJson(buf, kJsonCap, /*redactSecrets=*/false);
  bool ok = false;
  if (n > 0 && n < kJsonCap) {
    ok = NvsStore::putString(nvsns::kConfig, kKey, buf);
  } else {
    SH_LOGE(TAG, "config json overflow (%u bytes)", static_cast<unsigned>(n));
  }
  free(buf);

  if (ok) {
    Event e;
    e.type = EventType::ConfigChanged;
    EventBus::publish(e);
  }
  return ok;
}

void ConfigStore::resetToDefaults() {
  Lock lock;
  loadDefaults();
  NvsStore::clearNamespace(nvsns::kConfig);
  save();
}

bool ConfigStore::setDeviceName(const char* name) {
  if (!name || !name[0]) return false;
  {
    Lock lock;
    copyStr(g_device.deviceName, sizeof(g_device.deviceName), name);
  }
  return save();
}

bool ConfigStore::setChannelName(uint8_t index, const char* name) {
  if (index >= board::kChannelCount || !name || !name[0]) return false;
  {
    Lock lock;
    copyStr(g_channels[index].name, sizeof(g_channels[index].name), name);
  }
  return save();
}

bool ConfigStore::setCloud(const char* apiBase, const char* mqttHost, uint16_t port,
                           bool tls, const char* user, const char* pass) {
  {
    Lock lock;
    if (apiBase) copyStr(g_device.apiBase, sizeof(g_device.apiBase), apiBase);
    if (mqttHost) copyStr(g_device.mqttHost, sizeof(g_device.mqttHost), mqttHost);
    if (port) g_device.mqttPort = port;
    g_device.mqttTls = tls;
    if (user) copyStr(g_device.mqttUser, sizeof(g_device.mqttUser), user);
    if (pass) copyStr(g_device.mqttPass, sizeof(g_device.mqttPass), pass);
    g_device.cloudEnabled = (g_device.mqttHost[0] != '\0');
  }
  return save();
}

bool ConfigStore::setIds(const char* homeId, const char* roomId) {
  {
    Lock lock;
    if (homeId) copyStr(g_device.homeId, sizeof(g_device.homeId), homeId);
    if (roomId) copyStr(g_device.roomId, sizeof(g_device.roomId), roomId);
  }
  return save();
}

}  // namespace sh
