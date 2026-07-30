// ---------------------------------------------------------------------------
//  StatusLed.h - the onboard LED as a diagnosis tool you can read from across
//  the room, without a serial cable.
//
//   Booting        solid
//   Provisioning   fast blink   (SoftAP up, waiting for Wi-Fi credentials)
//   Connecting     slow blink
//   Online (LAN)   short flash every 3 s
//   Online (cloud) double flash every 3 s
//   Fault          rapid triple blink
//   OTA            continuous fast strobe
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

namespace sh {

enum class LedPattern : uint8_t {
  Off = 0,
  Solid,
  Booting,
  Provisioning,
  Connecting,
  OnlineLocal,
  OnlineCloud,
  Fault,
  Ota,
};

class StatusLed {
 public:
  static void begin();
  static void set(LedPattern pattern);
  static LedPattern pattern();

  /// Advances the blink state machine. Called from the system task.
  static void tick(uint32_t nowMs);
};

}  // namespace sh
