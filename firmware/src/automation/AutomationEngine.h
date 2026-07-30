// ---------------------------------------------------------------------------
//  AutomationEngine.h - event-driven rules, evaluated on the device.
//
//  Same principle as the scheduler: the cloud (and the Groq AI feature) is
//  where rules get AUTHORED; this is where they RUN. Once written, a rule
//  keeps working with no internet, no broker and no backend.
//
//  Loop safety is the interesting part. An action changes a relay, which emits
//  a RelayChanged event, which could re-trigger the same rule forever. Two
//  guards: a per-rule cooldown, and a recursion depth limit on the chain of
//  rule-caused events.
//
//  Rule shape (JSON array, persisted in NVS):
//    { "id": "porch-on-with-gate",
//      "enabled": true,
//      "trigger":   { "type":"relay", "channel":0, "state":true },
//      "conditions":[{ "type":"channel", "channel":2, "state":false },
//                    { "type":"timeBetween", "from":1080, "to":360 }],
//      "actions":   [{ "type":"relay", "channels":[3], "action":"on" },
//                    { "type":"timer", "channel":4, "seconds":300 }],
//      "cooldownSec": 5 }
//
//  Trigger types: relay | switchShort | switchLong | switchDouble | boot |
//                 online | offline
//  Condition types: channel | timeBetween | anyOn | allOff
//  Action types: relay | timer | allOff | allOn | group
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sh {

class AutomationEngine {
 public:
  static constexpr uint8_t kMaxRules = 20;
  static constexpr uint8_t kMaxActions = 4;
  static constexpr uint8_t kMaxConditions = 4;
  static constexpr uint8_t kMaxChainDepth = 3;

  static void begin();
  static void startTask();

  static size_t toJson(char* out, size_t cap);
  static bool applyJson(const char* json, char* err, size_t errCap);
  static bool save();

  static uint8_t ruleCount();
  static uint32_t firedCount();
  static uint32_t suppressedCount();  // loop guard hits, worth surfacing

 private:
  static void taskEntry(void* arg);
};

}  // namespace sh
