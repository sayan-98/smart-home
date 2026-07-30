// ---------------------------------------------------------------------------
//  Scheduler.h - on-device time rules. Runs entirely offline.
//
//  Schedules live in NVS and execute on the device, so once a schedule is
//  written it keeps firing with the internet down, the broker unreachable and
//  the backend asleep on a free tier. The cloud is where you AUTHOR them, not
//  where they run.
//
//  Honest limitation: there is no RTC. Until SNTP succeeds after a power cut
//  the device does not know the time, and the scheduler refuses to fire rather
//  than guessing (Loophole #4). `catchUp` handles the ordinary case of a
//  device that was briefly off and missed a transition.
//
//  Rule shape (JSON, persisted as an array):
//    { "id":"morning", "enabled":true, "channels":[0,3], "action":"on",
//      "minute": 420,              // minutes since local midnight
//      "days": 127,                // bitmask, bit0=Sunday .. bit6=Saturday
//      "catchUp": true }           // fire late if we missed it while off
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sh {

class Scheduler {
 public:
  static constexpr uint8_t kMaxRules = 24;

  static void begin();
  static void startTask();

  static size_t toJson(char* out, size_t cap);
  /// Replaces the whole rule set. Validated before anything is stored.
  static bool applyJson(const char* json, char* err, size_t errCap);
  static bool save();

  static uint8_t ruleCount();
  static uint32_t firedCount();

 private:
  static void taskEntry(void* arg);
};

}  // namespace sh
