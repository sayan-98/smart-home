// ---------------------------------------------------------------------------
//  RelayManager.h - the single source of truth for relay state.
//
//  Every actor (wall switch, app, Alexa, schedule, automation, cloud, timer)
//  submits a *command*; only this module touches a GPIO. That is what makes
//  "the state is correct everywhere" tractable: there is exactly one writer,
//  one revision counter, and one event stream.
//
//  Ordering guarantees:
//   - Commands are applied in the order they were queued.
//   - Each accepted change bumps a monotonic global revision and stamps the
//     channel with it. Downstream consumers use `rev` to discard stale cloud
//     commands and to suppress their own echoes.
//   - A gang operation (all-off, group, scene) staggers the coils by
//     kRelayGangStaggerMs so eight relays never inrush on the same
//     millisecond - while still servicing incoming commands during the
//     stagger, so a wall switch stays responsive.
//
//  All public methods are safe to call from any task. None may be called from
//  an ISR.
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config/Board.h"
#include "core/Types.h"

namespace sh {

class RelayManager {
 public:
  /// Drives every relay to OFF and configures the pins, then restores the
  /// per-channel state according to each channel's RestoreMode.
  /// Call this as early as possible in boot - before Wi-Fi, before anything
  /// slow - so the coils spend the least possible time in an undefined state.
  static void begin();

  /// Creates the FreeRTOS task that owns the GPIOs. Core 1, priority 4.
  static void startTask();

  // --- commands ------------------------------------------------------------

  static bool command(uint8_t channel, Action action, Source source);

  /// Cloud/sync path: applied only if `rev` is newer than what this channel
  /// already has. Prevents a delayed cloud message from undoing a wall switch
  /// press that happened after it was sent.
  static bool commandWithRev(uint8_t channel, Action action, Source source,
                             uint32_t rev);

  static bool commandMask(uint8_t mask, Action action, Source source);
  static bool commandAll(Action action, Source source);
  static bool commandGroup(uint8_t group, Action action, Source source);

  /// Turns the channel on and schedules an automatic off after `seconds`.
  /// seconds == 0 cancels a pending timer without changing the relay.
  static bool setAutoOff(uint8_t channel, uint32_t seconds, Source source);

  // --- state ---------------------------------------------------------------

  static ChannelState state(uint8_t channel);
  static bool     isOn(uint8_t channel);
  static uint8_t  stateMask();
  static uint32_t revision(uint8_t channel);
  static uint32_t globalRevision();

  /// Full snapshot in the shape the local API, MQTT and the app all consume.
  static size_t toJson(char* out, size_t cap);

  /// Reads the ACTUAL electrical level on a channel's relay pin.
  ///
  /// The pins are configured INPUT_OUTPUT so the output register can be read
  /// back. This is what separates "the firmware never drove the pin" from "the
  /// pin is driven correctly and the relay still did not move" - the second is
  /// always wiring or power, and without this you cannot tell which you have.
  static int rawPinLevel(uint8_t channel);

  /// Per-channel wiring report: expected level vs measured level.
  static size_t toPinDiagnosticsJson(char* out, size_t cap);

  /// One channel, same shape as an element of the snapshot array.
  static size_t channelToJson(uint8_t channel, char* out, size_t cap);

  /// Forces a pending NVS write immediately. Called before a deliberate
  /// reboot / OTA so the last few seconds of changes are not lost.
  static void flush();

  /// Drives everything OFF without persisting - used by factory reset and by
  /// the OTA path just before the image is swapped.
  static void safeShutdown(Source source);

 private:
  static void taskEntry(void* arg);
};

}  // namespace sh
