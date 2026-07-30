// ---------------------------------------------------------------------------
//  App.h - boot sequence and cross-service policy.
//
//  Boot order matters and is not arbitrary:
//    1. Logger, so everything after it can explain itself.
//    2. NVS + config, because the relay restore policy lives there.
//    3. RELAYS - before Wi-Fi, before the web server, before anything slow.
//       Every millisecond spent elsewhere first is a millisecond the coils
//       spend in an undefined state.
//    4. Switches, so the wall switches work even if the rest fails to start.
//    5. Everything else.
//
//  App also owns the policies that span services: what the status LED shows,
//  when an OTA image is considered proven, and what a factory reset means.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

#include "core/Types.h"

namespace sh {

class App {
 public:
  /// Full boot. Returns after every task has been created.
  static void boot();

  /// Housekeeping loop, driven from Arduino's loop().
  static void tick();

  static uint32_t bootCount();

 private:
  static void onEvent(const Event& event, void* ctx);
  static void performFactoryReset();
  static void updateLedPolicy();
};

}  // namespace sh
