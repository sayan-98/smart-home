// ---------------------------------------------------------------------------
//  TimeService.h - SNTP + POSIX timezone, with an honest "not synced" state.
//
//  There is no RTC on this board (Loophole #4). After a power cut with no
//  internet the device genuinely does not know the time, and schedules cannot
//  run. Rather than pretend, every timestamp is tagged `synced` so the app and
//  the backend can tell a real epoch from an uptime counter, and the scheduler
//  refuses to fire until the clock is trusted.
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace sh {

class TimeService {
 public:
  static void begin();

  /// Called when Wi-Fi comes up; (re)starts SNTP.
  static void onNetworkUp();
  static void onNetworkDown();

  static bool isSynced();

  /// Milliseconds since the Unix epoch. 0 when not synced.
  static uint64_t nowMs();

  /// Epoch ms when synced, uptime ms otherwise. Pair it with isSynced() or the
  /// `tsSynced` flag carried on every event.
  static uint64_t nowMsOrUptime();

  static time_t nowEpoch();

  /// Local wall-clock time per the configured POSIX TZ. False if not synced.
  static bool localTime(struct tm& out);

  /// "2026-07-30T22:14:05+05:30" or "unsynced".
  static size_t formatIso(char* out, size_t cap);

  /// Minutes since local midnight (0..1439), or -1 when not synced.
  /// This is what the scheduler compares against.
  static int minutesOfDay();

  /// 0 = Sunday .. 6 = Saturday, or -1 when not synced.
  static int dayOfWeek();

  /// Applies the timezone from ConfigStore. Safe to call after a config change.
  static void applyTimezone();

  /// Seconds since the last successful SNTP sync, or -1 if never.
  static int32_t secondsSinceSync();
};

}  // namespace sh
