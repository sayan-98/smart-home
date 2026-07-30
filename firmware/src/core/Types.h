// ---------------------------------------------------------------------------
//  Types.h - shared vocabulary. Kept dependency-free so every module can
//  include it without creating cycles, and so the native test build can too.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

namespace sh {

// ---------------------------------------------------------------------------
//  Who caused a state change. This is the backbone of "the state must be
//  correct everywhere": every RelayEvent carries its origin so the app, the
//  cloud and the audit log can all explain *why* a socket changed.
// ---------------------------------------------------------------------------
enum class Source : uint8_t {
  Unknown = 0,
  Physical,    // wall switch on the board
  App,         // phone/tablet, over LAN or cloud
  Alexa,       // voice (local Hue emulation or cloud skill)
  Schedule,    // on-device scheduler
  Automation,  // on-device rule engine
  Cloud,       // backend-initiated (sync, admin, AI-proposed + validated)
  Restore,     // applied at boot from persisted state
  Timer,       // auto-off timer expiry
  Factory,     // reset / safe state
};

const char* toString(Source s);
Source sourceFromString(const char* s);

/// What to do to a channel.
enum class Action : uint8_t { Off = 0, On = 1, Toggle = 2 };

/// What a channel should do when power comes back.
enum class RestoreMode : uint8_t {
  Off = 0,   // always come back OFF (safest, default for sockets)
  On = 1,    // always come back ON
  Last = 2,  // restore the state it had before the outage
};

/// How the physical switch behaves.
enum class SwitchMode : uint8_t {
  Momentary = 0,  // spring-return push button: press = toggle
  Latching = 1,   // standard modular rocker: any position change = toggle
};

// ---------------------------------------------------------------------------
//  Internal event bus payload. POD, cheap to copy, safe to queue.
// ---------------------------------------------------------------------------
enum class EventType : uint8_t {
  None = 0,
  RelayChanged,
  RelayCommandRejected,
  SwitchShortPress,
  SwitchLongPress,
  SwitchDoublePress,
  WifiConnected,
  WifiDisconnected,
  WifiProvisioned,
  TimeSynced,
  MqttConnected,
  MqttDisconnected,
  CloudRegistered,
  DeviceClaimed,
  ConfigChanged,
  OtaStarted,
  OtaProgress,
  OtaSucceeded,
  OtaFailed,
  FactoryReset,
  Heartbeat,
};

struct Event {
  EventType type = EventType::None;
  uint8_t   channel = 0xFF;  // 0xFF = not channel-specific
  bool      state = false;
  Source    source = Source::Unknown;
  uint32_t  rev = 0;    // monotonic revision of the channel after the change
  uint64_t  tsMs = 0;   // epoch ms when time is synced, else uptime ms
  bool      tsSynced = false;
  int32_t   value = 0;  // generic: OTA %, RSSI, error code, held ms
};

/// Snapshot of one channel, used by the local API, MQTT and the app.
struct ChannelState {
  bool     on = false;
  Source   source = Source::Unknown;
  uint32_t rev = 0;
  uint64_t changedAtMs = 0;
  bool     tsSynced = false;
  uint32_t autoOffAtMs = 0;  // 0 = no pending timer (uptime ms)
};

}  // namespace sh
