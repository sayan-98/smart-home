// ---------------------------------------------------------------------------
//  SwitchScanner.h - reads the physical wall switches and turns them into
//  relay commands.
//
//  Latency path: GPIO edge -> ISR -> task notify -> debounce -> relay command.
//  Nothing on this path touches the network, so a wall switch keeps working
//  identically whether the Wi-Fi is up, the broker is down, or the internet
//  has been out for a week.
//
//  The latching desync fix lives here. A rocker switch stays where you left
//  it, so after the app turns a socket off the switch is still physically in
//  the "on" position. Toggling relative to the SWITCH position would then do
//  nothing visible on the next flip. Instead any accepted position change
//  toggles relative to the RELAY's current state, which is what a person
//  expects: flip it, and the state changes.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

namespace sh {

class SwitchScanner {
 public:
  /// Configures the input pins and primes the debouncers to the levels the
  /// hardware is actually at, so boot never produces a phantom toggle.
  static void begin();

  /// Creates the FreeRTOS task. Core 1, priority 5 (above the relay task, so
  /// a press is queued the instant it is seen).
  static void startTask();

  /// Re-reads per-channel settings after a config change.
  static void reconfigure();

  /// Raw asserted state of a channel's switch, for diagnostics.
  static bool asserted(uint8_t channel);

  /// True while the BOOT button is being held toward a factory reset.
  static bool factoryResetPending();
  static uint32_t factoryResetHeldMs();

 private:
  static void taskEntry(void* arg);
};

}  // namespace sh
