#include "scheduler/Scheduler.h"

#include <ArduinoJson.h>
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "config/Board.h"
#include "core/EventBus.h"
#include "log/Logger.h"
#include "net/TimeService.h"
#include "relay/RelayManager.h"
#include "storage/NvsStore.h"

namespace sh {
namespace {

constexpr const char* TAG = "sched";
constexpr const char* kKey = "rules";
constexpr size_t kJsonCap = 3072;

struct Rule {
  char     id[20];
  bool     enabled;
  uint8_t  channelMask;
  Action   action;
  int16_t  minute;   // 0..1439, minutes since local midnight
  uint8_t  days;     // bit0 = Sunday .. bit6 = Saturday
  bool     catchUp;
  int16_t  lastFiredMinute;  // within lastFiredDay, -1 = never
  int16_t  lastFiredYday;
};

Rule g_rules[Scheduler::kMaxRules];
uint8_t g_count = 0;
uint32_t g_fired = 0;
TaskHandle_t g_task = nullptr;

Action actionFrom(const char* s, bool& ok) {
  ok = true;
  if (!s) { ok = false; return Action::Off; }
  if (strcmp(s, "on") == 0) return Action::On;
  if (strcmp(s, "off") == 0) return Action::Off;
  if (strcmp(s, "toggle") == 0) return Action::Toggle;
  ok = false;
  return Action::Off;
}

const char* actionName(Action a) {
  switch (a) {
    case Action::On:  return "on";
    case Action::Off: return "off";
    default:          return "toggle";
  }
}

void fire(Rule& r, int yday, int minute) {
  r.lastFiredMinute = static_cast<int16_t>(minute);
  r.lastFiredYday = static_cast<int16_t>(yday);
  g_fired++;
  SH_LOGI(TAG, "rule '%s' fired: mask=0x%02X %s", r.id,
          static_cast<unsigned>(r.channelMask), actionName(r.action));
  RelayManager::commandMask(r.channelMask, r.action, Source::Schedule);
}

}  // namespace

void Scheduler::begin() {
  memset(g_rules, 0, sizeof(g_rules));
  g_count = 0;

  char* buf = static_cast<char*>(malloc(kJsonCap));
  if (!buf) return;
  const size_t n = NvsStore::getString(nvsns::kSchedule, kKey, buf, kJsonCap, "");
  if (n > 0) {
    char err[80];
    if (!applyJson(buf, err, sizeof(err))) {
      SH_LOGE(TAG, "stored schedules rejected: %s", err);
      g_count = 0;
    }
  }
  free(buf);
  SH_LOGI(TAG, "%u schedule(s) loaded", static_cast<unsigned>(g_count));
}

bool Scheduler::applyJson(const char* json, char* err, size_t errCap) {
  if (err && errCap) err[0] = '\0';
  JsonDocument doc;
  if (deserializeJson(doc, json ? json : "")) {
    if (err) snprintf(err, errCap, "invalid json");
    return false;
  }
  JsonArrayConst arr = doc.is<JsonArrayConst>() ? doc.as<JsonArrayConst>()
                                                : doc["schedules"].as<JsonArrayConst>();
  if (arr.isNull()) {
    if (err) snprintf(err, errCap, "expected an array of schedules");
    return false;
  }

  // Build into a scratch set so a bad rule never half-replaces the live one.
  Rule staged[kMaxRules];
  memset(staged, 0, sizeof(staged));
  uint8_t n = 0;

  for (JsonObjectConst o : arr) {
    if (n >= kMaxRules) {
      if (err) snprintf(err, errCap, "too many schedules (max %u)", kMaxRules);
      return false;
    }
    Rule& r = staged[n];

    const char* id = o["id"] | "";
    if (!id[0]) {
      if (err) snprintf(err, errCap, "schedule %u has no id", n);
      return false;
    }
    strncpy(r.id, id, sizeof(r.id) - 1);

    const int minute = o["minute"] | -1;
    if (minute < 0 || minute > 1439) {
      if (err) snprintf(err, errCap, "schedule '%s': minute must be 0..1439", r.id);
      return false;
    }
    r.minute = static_cast<int16_t>(minute);

    bool ok = false;
    r.action = actionFrom(o["action"], ok);
    if (!ok) {
      if (err) snprintf(err, errCap, "schedule '%s': action must be on|off|toggle", r.id);
      return false;
    }

    r.channelMask = 0;
    JsonArrayConst chans = o["channels"].as<JsonArrayConst>();
    if (chans.isNull()) {
      if (err) snprintf(err, errCap, "schedule '%s': channels required", r.id);
      return false;
    }
    for (JsonVariantConst c : chans) {
      const int ch = c.as<int>();
      if (ch < 0 || ch >= board::kChannelCount) {
        if (err) snprintf(err, errCap, "schedule '%s': channel %d out of range", r.id, ch);
        return false;
      }
      r.channelMask |= static_cast<uint8_t>(1u << ch);
    }
    if (r.channelMask == 0) {
      if (err) snprintf(err, errCap, "schedule '%s': no channels selected", r.id);
      return false;
    }

    r.days = static_cast<uint8_t>(o["days"] | 0x7F);
    if ((r.days & 0x7F) == 0) {
      if (err) snprintf(err, errCap, "schedule '%s': no days selected", r.id);
      return false;
    }
    r.enabled = o["enabled"] | true;
    r.catchUp = o["catchUp"] | false;
    r.lastFiredMinute = -1;
    r.lastFiredYday = -1;
    n++;
  }

  memcpy(g_rules, staged, sizeof(staged));
  g_count = n;
  SH_LOGI(TAG, "%u schedule(s) applied", static_cast<unsigned>(n));
  return true;
}

size_t Scheduler::toJson(char* out, size_t cap) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < g_count; ++i) {
    const Rule& r = g_rules[i];
    JsonObject o = arr.add<JsonObject>();
    o["id"] = r.id;
    o["enabled"] = r.enabled;
    o["minute"] = r.minute;
    o["days"] = r.days;
    o["action"] = actionName(r.action);
    o["catchUp"] = r.catchUp;
    JsonArray ch = o["channels"].to<JsonArray>();
    for (uint8_t c = 0; c < board::kChannelCount; ++c) {
      if (r.channelMask & (1u << c)) ch.add(c);
    }
  }
  return serializeJson(doc, out, cap);
}

bool Scheduler::save() {
  char* buf = static_cast<char*>(malloc(kJsonCap));
  if (!buf) return false;
  const size_t n = toJson(buf, kJsonCap);
  const bool ok = n > 0 && n < kJsonCap &&
                  NvsStore::putString(nvsns::kSchedule, kKey, buf);
  free(buf);
  return ok;
}

uint8_t Scheduler::ruleCount() { return g_count; }
uint32_t Scheduler::firedCount() { return g_fired; }

void Scheduler::startTask() {
  if (g_task) return;
  xTaskCreatePinnedToCore(&Scheduler::taskEntry, "sched", 4096, nullptr, 2, &g_task, 1);
}

void Scheduler::taskEntry(void* /*arg*/) {
  esp_task_wdt_add(nullptr);
  int lastMinute = -1;
  bool warnedUnsynced = false;

  for (;;) {
    esp_task_wdt_reset();

    if (!TimeService::isSynced()) {
      // Firing on a guessed clock is worse than not firing. Say so once, then
      // stay quiet until the clock arrives.
      if (!warnedUnsynced && g_count > 0) {
        SH_LOGW(TAG, "clock not set - %u schedule(s) on hold",
                static_cast<unsigned>(g_count));
        warnedUnsynced = true;
      }
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    if (warnedUnsynced) {
      SH_LOGI(TAG, "clock available, schedules active");
      warnedUnsynced = false;
    }

    struct tm now {};
    if (!TimeService::localTime(now)) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    const int minute = now.tm_hour * 60 + now.tm_min;
    const int wday = now.tm_wday;
    const int yday = now.tm_yday;

    if (minute != lastMinute) {
      const bool minuteJumped = lastMinute >= 0 && minute != ((lastMinute + 1) % 1440);
      lastMinute = minute;

      for (uint8_t i = 0; i < g_count; ++i) {
        Rule& r = g_rules[i];
        if (!r.enabled) continue;
        if (!(r.days & (1u << wday))) continue;
        if (r.lastFiredYday == yday && r.lastFiredMinute == r.minute) continue;

        if (r.minute == minute) {
          fire(r, yday, minute);
        } else if (r.catchUp && minuteJumped && r.minute < minute &&
                   r.lastFiredYday != yday) {
          // We were off (or the clock jumped) across this rule's time today.
          SH_LOGI(TAG, "rule '%s' catching up (missed %02d:%02d)", r.id,
                  r.minute / 60, r.minute % 60);
          fire(r, yday, minute);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

}  // namespace sh
