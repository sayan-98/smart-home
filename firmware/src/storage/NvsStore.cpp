#include "storage/NvsStore.h"

#include <Preferences.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

#include "log/Logger.h"

namespace sh {
namespace {

constexpr const char* TAG = "nvs";

/// RAII open/close so no code path can leak a handle.
struct Scope {
  Preferences prefs;
  bool ok = false;
  Scope(const char* ns, bool readOnly) { ok = prefs.begin(ns, readOnly); }
  ~Scope() {
    if (ok) prefs.end();
  }
};

}  // namespace

namespace {
const char* const kAllNamespaces[] = {
    nvsns::kConfig, nvsns::kRelay,      nvsns::kWifi,  nvsns::kSecurity,
    nvsns::kSchedule, nvsns::kAutomation, nvsns::kSystem};
}  // namespace

void NvsStore::begin() {
  uint8_t created = 0;
  for (const char* ns : kAllNamespaces) {
    // Opening read-WRITE creates the namespace if it is missing. Every later
    // read-only open then succeeds instead of logging NOT_FOUND.
    Scope s(ns, false);
    if (s.ok) created++;
  }
  SH_LOGI(TAG, "%u/%u namespaces ready, %u free entries",
          static_cast<unsigned>(created),
          static_cast<unsigned>(sizeof(kAllNamespaces) / sizeof(kAllNamespaces[0])),
          static_cast<unsigned>(freeEntries()));
}

bool NvsStore::putString(const char* ns, const char* key, const char* value) {
  Scope s(ns, false);
  if (!s.ok) return false;
  return s.prefs.putString(key, value ? value : "") > 0 ||
         (value && value[0] == '\0');
}

size_t NvsStore::getString(const char* ns, const char* key, char* out, size_t cap,
                           const char* fallback) {
  if (!out || cap == 0) return 0;
  Scope s(ns, true);
  if (!s.ok || !s.prefs.isKey(key)) {
    strncpy(out, fallback ? fallback : "", cap - 1);
    out[cap - 1] = '\0';
    return strlen(out);
  }
  const size_t n = s.prefs.getString(key, out, cap);
  out[cap - 1] = '\0';
  return n;
}

bool NvsStore::putBlob(const char* ns, const char* key, const void* data, size_t len) {
  Scope s(ns, false);
  if (!s.ok) return false;
  return s.prefs.putBytes(key, data, len) == len;
}

size_t NvsStore::getBlob(const char* ns, const char* key, void* out, size_t cap) {
  Scope s(ns, true);
  if (!s.ok || !s.prefs.isKey(key)) return 0;
  return s.prefs.getBytes(key, out, cap);
}

bool NvsStore::putU32(const char* ns, const char* key, uint32_t value) {
  Scope s(ns, false);
  if (!s.ok) return false;
  return s.prefs.putUInt(key, value) == sizeof(uint32_t);
}

uint32_t NvsStore::getU32(const char* ns, const char* key, uint32_t fallback) {
  Scope s(ns, true);
  if (!s.ok) return fallback;
  return s.prefs.getUInt(key, fallback);
}

bool NvsStore::putU64(const char* ns, const char* key, uint64_t value) {
  Scope s(ns, false);
  if (!s.ok) return false;
  return s.prefs.putULong64(key, value) == sizeof(uint64_t);
}

uint64_t NvsStore::getU64(const char* ns, const char* key, uint64_t fallback) {
  Scope s(ns, true);
  if (!s.ok) return fallback;
  return s.prefs.getULong64(key, fallback);
}

bool NvsStore::putBool(const char* ns, const char* key, bool value) {
  Scope s(ns, false);
  if (!s.ok) return false;
  return s.prefs.putBool(key, value) == sizeof(uint8_t);
}

bool NvsStore::getBool(const char* ns, const char* key, bool fallback) {
  Scope s(ns, true);
  if (!s.ok) return fallback;
  return s.prefs.getBool(key, fallback);
}

bool NvsStore::remove(const char* ns, const char* key) {
  Scope s(ns, false);
  if (!s.ok) return false;
  return s.prefs.remove(key);
}

bool NvsStore::clearNamespace(const char* ns) {
  Scope s(ns, false);
  if (!s.ok) return false;
  return s.prefs.clear();
}

void NvsStore::factoryWipe() {
  for (const char* ns : kAllNamespaces) {
    if (clearNamespace(ns)) {
      SH_LOGW(TAG, "wiped namespace %s", ns);
    }
  }
}

size_t NvsStore::freeEntries() {
  nvs_stats_t stats{};
  if (nvs_get_stats(nullptr, &stats) != ESP_OK) return 0;
  return stats.free_entries;
}

}  // namespace sh
