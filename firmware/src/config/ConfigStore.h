// ---------------------------------------------------------------------------
//  ConfigStore.h - runtime configuration, persisted to NVS as JSON.
//
//  JSON rather than a packed struct because the same shape is served by the
//  local REST API and the app; one representation, no translation layer, and
//  adding a field later does not invalidate what is already on the device.
//  `version` drives migration.
//
//  Threading: readers take const refs and read POD fields (cheap, no lock).
//  Every mutation goes through a setter that holds the mutex and marks dirty;
//  save() is called from the config owner, not from the relay hot path.
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config/Board.h"
#include "core/Types.h"

namespace sh {

/// What a long / double press does, beyond toggling its own channel.
enum class GestureAction : uint8_t {
  None = 0,
  ToggleOwn = 1,    // same as a short press
  AllOff = 2,       // every channel on this device off
  AllOn = 3,
  GroupToggle = 4,  // toggle every channel sharing this channel's group id
};

struct ChannelConfig {
  char name[24];
  char icon[16];

  bool        relayEnabled;
  RestoreMode restore;

  bool       switchEnabled;
  SwitchMode switchMode;
  bool       switchInverted;

  uint16_t debounceMs;
  uint16_t longPressMs;    // 0 = gesture disabled (keeps short press instant)
  uint16_t doublePressMs;  // 0 = gesture disabled (keeps short press instant)

  GestureAction longPressAction;
  GestureAction doublePressAction;

  uint8_t  group;       // 0 = ungrouped
  uint32_t autoOffSec;  // 0 = no auto-off timer
};

struct DeviceConfig {
  uint16_t version;

  char deviceName[32];
  char homeId[40];
  char roomId[40];

  char timezone[40];   // POSIX TZ string. India: "IST-5:30"
  char ntpServer[48];

  char     mqttHost[72];
  uint16_t mqttPort;
  bool     mqttTls;
  char     mqttUser[40];
  char     mqttPass[72];

  char apiBase[104];  // https://<your-app>.onrender.com

  bool alexaEnabled;  // local Hue-bridge emulation for Echo
  /// true  -> advertise each channel as an "On/Off plug-in unit" (correct for
  ///          sockets; Alexa shows them as plugs).
  /// false -> advertise as a dimmable light. Fallback for Echo firmware that
  ///          refuses to discover plugs; brightness is ignored beyond on/off.
  bool alexaAsPlug;
  bool localApiEnabled;  // on-device REST + WebSocket
  bool cloudEnabled;     // MQTT + REST to the backend
  uint8_t logLevel;
};

class ConfigStore {
 public:
  static constexpr uint16_t kSchemaVersion = 1;

  /// Loads from NVS, applying defaults for anything missing. Never fails:
  /// a corrupt or absent blob yields a safe default configuration.
  static void begin();

  static const DeviceConfig& device();
  static const ChannelConfig& channel(uint8_t index);
  static uint8_t channelCount() { return board::kChannelCount; }

  /// Applies a partial JSON patch (the shape the REST API accepts) and saves.
  /// Returns false with `err` populated when validation fails - invalid config
  /// is rejected wholesale rather than partially applied.
  static bool applyJson(const char* json, char* err, size_t errCap);

  /// Serialises the full configuration. Secrets (mqttPass) are redacted when
  /// `redactSecrets` is true - which is what the REST API uses.
  static size_t toJson(char* out, size_t cap, bool redactSecrets);

  static bool save();
  static void resetToDefaults();

  /// Convenience setters used by provisioning / claim flows.
  static bool setDeviceName(const char* name);
  static bool setChannelName(uint8_t index, const char* name);
  static bool setCloud(const char* apiBase, const char* mqttHost, uint16_t port,
                       bool tls, const char* user, const char* pass);
  static bool setIds(const char* homeId, const char* roomId);

 private:
  static void loadDefaults();
};

}  // namespace sh
