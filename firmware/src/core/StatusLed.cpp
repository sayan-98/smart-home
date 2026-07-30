#include "core/StatusLed.h"

#include <Arduino.h>

#include "config/Board.h"

namespace sh {
namespace {

LedPattern g_pattern = LedPattern::Booting;
uint32_t g_phaseStartMs = 0;
uint8_t g_step = 0;

inline void write(bool on) {
  if (board::kStatusLedPin < 0) return;
  const bool level = board::kStatusLedActiveLow ? !on : on;
  digitalWrite(board::kStatusLedPin, level ? HIGH : LOW);
}

/// Each pattern is a cycle of (durationMs, on) steps.
struct Step {
  uint16_t ms;
  bool on;
};

const Step kProvisioning[] = {{120, true}, {120, false}};
const Step kConnecting[]   = {{500, true}, {500, false}};
const Step kOnlineLocal[]  = {{60, true}, {2940, false}};
const Step kOnlineCloud[]  = {{60, true}, {160, false}, {60, true}, {2720, false}};
const Step kFault[]        = {{80, true}, {80, false}, {80, true}, {80, false},
                              {80, true}, {600, false}};
const Step kOta[]          = {{50, true}, {50, false}};

struct Sequence {
  const Step* steps;
  uint8_t count;
};

Sequence sequenceFor(LedPattern p) {
  switch (p) {
    case LedPattern::Provisioning: return {kProvisioning, 2};
    case LedPattern::Connecting:   return {kConnecting, 2};
    case LedPattern::OnlineLocal:  return {kOnlineLocal, 2};
    case LedPattern::OnlineCloud:  return {kOnlineCloud, 4};
    case LedPattern::Fault:        return {kFault, 6};
    case LedPattern::Ota:          return {kOta, 2};
    default:                       return {nullptr, 0};
  }
}

}  // namespace

void StatusLed::begin() {
  if (board::kStatusLedPin < 0) return;
  pinMode(board::kStatusLedPin, OUTPUT);
  g_pattern = LedPattern::Booting;
  g_phaseStartMs = millis();
  g_step = 0;
  write(true);
}

void StatusLed::set(LedPattern pattern) {
  if (pattern == g_pattern) return;
  g_pattern = pattern;
  g_step = 0;
  g_phaseStartMs = millis();
  if (pattern == LedPattern::Off) write(false);
  if (pattern == LedPattern::Solid || pattern == LedPattern::Booting) write(true);
}

LedPattern StatusLed::pattern() { return g_pattern; }

void StatusLed::tick(uint32_t nowMs) {
  const Sequence seq = sequenceFor(g_pattern);
  if (!seq.steps) return;  // Off / Solid / Booting are static

  const Step& current = seq.steps[g_step % seq.count];
  if (static_cast<uint32_t>(nowMs - g_phaseStartMs) < current.ms) return;

  g_step = static_cast<uint8_t>((g_step + 1) % seq.count);
  g_phaseStartMs = nowMs;
  write(seq.steps[g_step].on);
}

}  // namespace sh
