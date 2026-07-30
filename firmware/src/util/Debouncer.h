// ---------------------------------------------------------------------------
//  Debouncer.h - leading-edge contact debounce.
//
//  Why leading-edge and not the usual "wait until stable for N ms":
//  a wall switch must feel instant. Waiting for stability adds the whole
//  debounce window to the perceived latency. Here the FIRST edge is accepted
//  immediately and further edges are ignored for a lockout window.
//
//  Self-correcting: if the contact bounces back to its original level and
//  stays there, the level is re-accepted once the lockout expires, so we never
//  get stuck reporting a level the hardware isn't actually at.
//
//  Hardware-free on purpose - compiled by the native test build.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

namespace sh {

class Debouncer {
 public:
  Debouncer() = default;

  void configure(uint16_t lockoutMs, bool initialLevel) {
    lockoutMs_ = lockoutMs;
    level_ = initialLevel;
    lastAcceptMs_ = 0;
    primed_ = false;
  }

  /// Feed the raw pin level. Returns true when a transition was accepted.
  /// `nowMs` is a free-running millisecond counter.
  bool update(bool raw, uint32_t nowMs) {
    if (!primed_) {
      // First call after configure(): adopt reality without reporting a change.
      // Back-date the lockout so priming does not swallow a real edge that
      // arrives immediately afterwards - at boot, or right after a config
      // change, the next flip must still act instantly. Unsigned wrap-around
      // here is intentional and harmless: the comparison below is modular.
      primed_ = true;
      level_ = raw;
      lastAcceptMs_ = nowMs - lockoutMs_;
      return false;
    }
    if (raw == level_) return false;
    if (static_cast<uint32_t>(nowMs - lastAcceptMs_) < lockoutMs_) return false;

    level_ = raw;
    lastAcceptMs_ = nowMs;
    return true;
  }

  bool level() const { return level_; }
  uint32_t lastAcceptMs() const { return lastAcceptMs_; }

 private:
  uint16_t lockoutMs_ = 25;
  uint32_t lastAcceptMs_ = 0;
  bool     level_ = true;
  bool     primed_ = false;
};

}  // namespace sh
