// ---------------------------------------------------------------------------
//  Backoff.h - exponential reconnect backoff with jitter.
//
//  Jitter matters: without it, a fleet of devices that all lost the router
//  reconnect in lockstep and hammer the AP (and later, the broker) in waves.
//  Hardware-free - compiled by the native test build.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

namespace sh {

class Backoff {
 public:
  void configure(uint32_t baseMs, uint32_t maxMs, uint8_t jitterPct = 20) {
    baseMs_ = baseMs ? baseMs : 1;
    maxMs_ = maxMs;
    jitterPct_ = jitterPct > 90 ? 90 : jitterPct;
    reset();
  }

  void reset() {
    currentMs_ = baseMs_;
    attempts_ = 0;
  }

  /// Next delay, then doubles for the following call.
  uint32_t next(uint32_t entropy) {
    const uint32_t base = currentMs_;
    if (currentMs_ < maxMs_) {
      currentMs_ = (currentMs_ > maxMs_ / 2) ? maxMs_ : currentMs_ * 2;
    }
    if (attempts_ < 0xFF) attempts_++;

    if (jitterPct_ == 0) return base;
    const uint32_t span = (base / 100u) * jitterPct_;
    if (span == 0) return base;
    // Symmetric jitter around `base`, clamped at >= 1 ms.
    const uint32_t delta = entropy % (span * 2u + 1u);
    const int64_t v = static_cast<int64_t>(base) + static_cast<int64_t>(delta) - span;
    return v < 1 ? 1u : static_cast<uint32_t>(v);
  }

  uint32_t currentMs() const { return currentMs_; }
  uint8_t attempts() const { return attempts_; }

 private:
  uint32_t baseMs_ = 1000;
  uint32_t maxMs_ = 60000;
  uint32_t currentMs_ = 1000;
  uint8_t  jitterPct_ = 20;
  uint8_t  attempts_ = 0;
};

}  // namespace sh
