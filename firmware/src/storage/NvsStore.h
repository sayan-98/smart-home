// ---------------------------------------------------------------------------
//  NvsStore.h - typed, namespaced wrapper over ESP32 NVS (Preferences).
//
//  Every call opens and closes the namespace. That is slightly slow but it is
//  crash-safe and keeps no handles open across a watchdog reset. All writes in
//  this firmware are infrequent by design (relay state is debounced 2 s before
//  it reaches here - Loophole #5), so the cost never lands on a hot path.
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sh {

namespace nvsns {
inline constexpr const char* kConfig = "sh-cfg";    // device + channel config
inline constexpr const char* kRelay = "sh-rly";     // persisted relay states
inline constexpr const char* kWifi = "sh-wifi";     // saved networks
inline constexpr const char* kSecurity = "sh-sec";  // api key, replay counter
inline constexpr const char* kSchedule = "sh-sch";  // schedules
inline constexpr const char* kAutomation = "sh-aut";  // rules
inline constexpr const char* kSystem = "sh-sys";    // boot counters, ota flags
}  // namespace nvsns

class NvsStore {
 public:
  static bool putString(const char* ns, const char* key, const char* value);
  static size_t getString(const char* ns, const char* key, char* out, size_t cap,
                          const char* fallback = "");

  static bool putBlob(const char* ns, const char* key, const void* data, size_t len);
  static size_t getBlob(const char* ns, const char* key, void* out, size_t cap);

  static bool putU32(const char* ns, const char* key, uint32_t value);
  static uint32_t getU32(const char* ns, const char* key, uint32_t fallback = 0);

  static bool putU64(const char* ns, const char* key, uint64_t value);
  static uint64_t getU64(const char* ns, const char* key, uint64_t fallback = 0);

  static bool putBool(const char* ns, const char* key, bool value);
  static bool getBool(const char* ns, const char* key, bool fallback = false);

  static bool remove(const char* ns, const char* key);
  static bool clearNamespace(const char* ns);

  /// Wipes every namespace this firmware owns. Used by factory reset.
  /// Does NOT touch otadata - a factory reset must not un-validate the
  /// running image.
  static void factoryWipe();

  /// Free NVS entries, for the diagnostics payload.
  static size_t freeEntries();
};

}  // namespace sh
