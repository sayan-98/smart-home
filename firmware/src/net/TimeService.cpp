#include "net/TimeService.h"

#include <Arduino.h>
#include <esp_sntp.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "config/ConfigStore.h"
#include "core/EventBus.h"
#include "log/Logger.h"

namespace sh {
namespace {

constexpr const char* TAG = "time";

/// Any epoch below this is obviously the 1970 default, not a real sync.
/// 2024-01-01T00:00:00Z.
constexpr time_t kSaneEpochFloor = 1704067200;

volatile bool g_synced = false;
volatile uint32_t g_lastSyncUptimeMs = 0;
bool g_sntpStarted = false;

void onSntpSync(struct timeval* /*tv*/) {
  const bool first = !g_synced;
  g_synced = true;
  g_lastSyncUptimeMs = millis();

  char buf[40];
  TimeService::formatIso(buf, sizeof(buf));
  SH_LOGI(TAG, "clock synced: %s", buf);

  if (first) {
    Event e;
    e.type = EventType::TimeSynced;
    e.tsMs = TimeService::nowMs();
    e.tsSynced = true;
    EventBus::publish(e);
  }
}

}  // namespace

void TimeService::begin() {
  applyTimezone();
  // If the RTC survived a soft reset the time may already be valid.
  if (time(nullptr) >= kSaneEpochFloor) {
    g_synced = true;
    g_lastSyncUptimeMs = millis();
    SH_LOGI(TAG, "clock already valid across reset");
  } else {
    SH_LOGW(TAG, "clock not set - schedules are on hold until SNTP succeeds");
  }
}

void TimeService::applyTimezone() {
  const char* tz = ConfigStore::device().timezone;
  if (tz && tz[0]) {
    setenv("TZ", tz, 1);
    tzset();
    SH_LOGI(TAG, "timezone set to %s", tz);
  }
}

void TimeService::onNetworkUp() {
  applyTimezone();
  const DeviceConfig& cfg = ConfigStore::device();
  const char* server = (cfg.ntpServer[0] != '\0') ? cfg.ntpServer : "pool.ntp.org";

  if (g_sntpStarted) {
    esp_sntp_stop();
    g_sntpStarted = false;
  }

  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, server);
  esp_sntp_setservername(1, "time.google.com");
  esp_sntp_setservername(2, "time.cloudflare.com");
  sntp_set_time_sync_notification_cb(&onSntpSync);
  // Smooth adjustment would make the clock crawl to the right value over
  // minutes; schedules want it correct now.
  sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
  esp_sntp_init();
  g_sntpStarted = true;

  SH_LOGI(TAG, "SNTP started (%s)", server);
}

void TimeService::onNetworkDown() {
  // Deliberately keep `g_synced` true: the clock keeps running off the ESP32's
  // oscillator and stays good enough for hours. Only a power loss really
  // invalidates it, and that path goes through begin().
  if (g_sntpStarted) {
    esp_sntp_stop();
    g_sntpStarted = false;
  }
}

bool TimeService::isSynced() { return g_synced && time(nullptr) >= kSaneEpochFloor; }

uint64_t TimeService::nowMs() {
  if (!isSynced()) return 0;
  struct timeval tv {};
  gettimeofday(&tv, nullptr);
  return static_cast<uint64_t>(tv.tv_sec) * 1000ULL +
         static_cast<uint64_t>(tv.tv_usec / 1000);
}

uint64_t TimeService::nowMsOrUptime() {
  const uint64_t ms = nowMs();
  return ms ? ms : static_cast<uint64_t>(millis());
}

time_t TimeService::nowEpoch() { return isSynced() ? time(nullptr) : 0; }

bool TimeService::localTime(struct tm& out) {
  if (!isSynced()) return false;
  const time_t t = time(nullptr);
  localtime_r(&t, &out);
  return true;
}

size_t TimeService::formatIso(char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  struct tm tm_ {};
  if (!localTime(tm_)) {
    strncpy(out, "unsynced", cap - 1);
    out[cap - 1] = '\0';
    return strlen(out);
  }
  // %z gives "+0530"; splice in the colon so the app and Postgres both accept it.
  char base[32];
  strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S%z", &tm_);
  const size_t n = strlen(base);
  if (n >= 5) {
    char tail[8];
    snprintf(tail, sizeof(tail), "%c%c%c:%c%c", base[n - 5], base[n - 4],
             base[n - 3], base[n - 2], base[n - 1]);
    base[n - 5] = '\0';
    snprintf(out, cap, "%s%s", base, tail);
  } else {
    strncpy(out, base, cap - 1);
    out[cap - 1] = '\0';
  }
  return strlen(out);
}

int TimeService::minutesOfDay() {
  struct tm tm_ {};
  if (!localTime(tm_)) return -1;
  return tm_.tm_hour * 60 + tm_.tm_min;
}

int TimeService::dayOfWeek() {
  struct tm tm_ {};
  if (!localTime(tm_)) return -1;
  return tm_.tm_wday;
}

int32_t TimeService::secondsSinceSync() {
  if (!g_synced) return -1;
  return static_cast<int32_t>((millis() - g_lastSyncUptimeMs) / 1000u);
}

}  // namespace sh
