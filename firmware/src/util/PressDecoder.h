// ---------------------------------------------------------------------------
//  PressDecoder.h - turns debounced momentary-switch edges into gestures.
//
//  Latency policy (important, and deliberate):
//    * Both gestures disabled (the default)  -> Short fires on the PRESS edge.
//      Zero added latency; the relay clicks the instant you touch the switch.
//    * Either gesture enabled                -> Short is deferred to the
//      RELEASE edge (and further, by the double-press window). This is
//      unavoidable: you cannot know a press is "short" until it ends.
//
//  So the cost of gestures is paid only by channels that opt into them.
//  Hardware-free on purpose - compiled by the native test build.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

namespace sh {

enum class PressResult : uint8_t { None = 0, Short, Long, Double };

class PressDecoder {
 public:
  void configure(uint16_t longPressMs, uint16_t doublePressMs) {
    longMs_ = longPressMs;
    doubleMs_ = doublePressMs;
    reset();
  }

  void reset() {
    held_ = false;
    longFired_ = false;
    suppressRelease_ = false;
    shortPending_ = false;
    pressedAtMs_ = 0;
    releasedAtMs_ = 0;
  }

  bool instantMode() const { return longMs_ == 0 && doubleMs_ == 0; }

  /// Feed an accepted (already debounced) edge.
  /// `asserted` == true means the contact is now closed / button down.
  PressResult onEdge(bool asserted, uint32_t nowMs) {
    if (asserted) {
      held_ = true;
      longFired_ = false;
      suppressRelease_ = false;
      pressedAtMs_ = nowMs;

      if (shortPending_ && static_cast<uint32_t>(nowMs - releasedAtMs_) <= doubleMs_) {
        shortPending_ = false;
        suppressRelease_ = true;  // the release of the 2nd press emits nothing
        return PressResult::Double;
      }
      return instantMode() ? PressResult::Short : PressResult::None;
    }

    // release edge
    held_ = false;
    releasedAtMs_ = nowMs;
    if (suppressRelease_ || longFired_ || instantMode()) {
      suppressRelease_ = false;
      return PressResult::None;
    }
    if (doubleMs_ > 0) {
      shortPending_ = true;  // wait and see whether a second press arrives
      return PressResult::None;
    }
    return PressResult::Short;
  }

  /// Call periodically. Emits long-press while still held, and the deferred
  /// short-press once the double-press window has closed.
  PressResult tick(uint32_t nowMs) {
    if (held_ && longMs_ > 0 && !longFired_ &&
        static_cast<uint32_t>(nowMs - pressedAtMs_) >= longMs_) {
      longFired_ = true;
      suppressRelease_ = true;
      return PressResult::Long;
    }
    if (shortPending_ && static_cast<uint32_t>(nowMs - releasedAtMs_) > doubleMs_) {
      shortPending_ = false;
      return PressResult::Short;
    }
    return PressResult::None;
  }

  bool held() const { return held_; }

 private:
  uint16_t longMs_ = 0;
  uint16_t doubleMs_ = 0;
  uint32_t pressedAtMs_ = 0;
  uint32_t releasedAtMs_ = 0;
  bool held_ = false;
  bool longFired_ = false;
  bool suppressRelease_ = false;
  bool shortPending_ = false;
};

}  // namespace sh
