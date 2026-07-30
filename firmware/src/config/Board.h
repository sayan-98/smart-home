// ---------------------------------------------------------------------------
//  Board.h - hardware contract for the ESP32 DevKit V1 (30-pin) + 8-ch relay.
//
//  This is the ONLY file that knows about physical pins. Everything else
//  addresses channels 0..7. Porting to a different board = edit this file.
//
//  !! HARDWARE PREREQUISITES (see docs/WIRING.md) !!
//   - 10 kOhm pull-up from every relay IN line to 3V3. Without them all eight
//     relays energise for ~300 ms on every boot/brownout, because GPIOs are
//     floating inputs until setup() runs. Firmware cannot fix this.
//   - JD-VCC jumper REMOVED, coils fed from a separate 5 V / 2 A supply.
//   - Wall switches wired as DRY CONTACTS (GPIO <-> GND). Never mains.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

namespace sh {
namespace board {

/// Number of relay channels on this hardware.
inline constexpr uint8_t kChannelCount = 8;

/// Relay drive pins, channel 0..7 -> R1..R8.
/// Deliberately avoids strapping pins that boot LOW (0, 2, 12, 15) because a
/// LOW on an active-LOW module means "relay ON".
/// GPIO16/17 are exposed on this board as RX2/TX2 - using them costs Serial2.
inline constexpr int8_t kRelayPins[kChannelCount] = {23, 22, 21, 19, 18, 5, 17, 16};

/// Wall-switch input pins, channel 0..7 -> S1..S8.
/// All support an internal pull-up; the switch shorts the pin to GND.
inline constexpr int8_t kSwitchPins[kChannelCount] = {13, 14, 27, 26, 25, 33, 32, 4};

/// Relay module polarity. Blue SONGLE SRD-05VDC boards are active LOW:
/// driving the pin LOW energises the coil and closes COM->NO.
inline constexpr bool kRelayActiveLow = true;

/// Switch electrical polarity: with INPUT_PULLUP the pin idles HIGH and the
/// closed contact pulls it LOW, so "asserted" == LOW.
inline constexpr bool kSwitchActiveLow = true;

/// Onboard blue LED. Used for status blink codes.
inline constexpr int8_t kStatusLedPin = 2;
inline constexpr bool   kStatusLedActiveLow = false;

/// BOOT button. Held for kFactoryResetHoldMs at any time -> factory reset.
inline constexpr int8_t  kFactoryResetPin    = 0;
inline constexpr uint32_t kFactoryResetHoldMs = 10000;

// --- timing defaults (overridable per channel at runtime) ------------------

/// Leading-edge debounce lockout. The FIRST edge acts immediately; further
/// edges inside this window are ignored. Gives <2 ms perceived latency while
/// still rejecting contact bounce.
inline constexpr uint16_t kDefaultDebounceMs = 25;

/// 0 disables the gesture. Long-press fires while still held.
inline constexpr uint16_t kDefaultLongPressMs = 0;

/// 0 disables double-press AND keeps single-press instant. Setting a non-zero
/// value necessarily delays single-press by this much - that is the trade-off.
inline constexpr uint16_t kDefaultDoublePressMs = 0;

/// How long a relay change waits before being committed to NVS. Protects the
/// ~100k-cycle flash endurance from a chattering channel (Loophole #5).
inline constexpr uint32_t kRelayPersistDebounceMs = 2000;

/// Minimum time between two coil transitions on the same channel. Guards the
/// contacts against a runaway automation cycling the relay at full speed.
inline constexpr uint32_t kRelayMinIntervalMs = 150;

/// Stagger applied when several channels switch at once (scene / all-off), so
/// eight coils never inrush on the same millisecond.
inline constexpr uint32_t kRelayGangStaggerMs = 40;

}  // namespace board
}  // namespace sh
