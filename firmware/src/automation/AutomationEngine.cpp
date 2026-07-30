#include "automation/AutomationEngine.h"

#include <ArduinoJson.h>
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
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

constexpr const char* TAG = "auto";
constexpr const char* kKey = "rules";
constexpr size_t kJsonCap = 4096;

enum class TriggerType : uint8_t {
  None = 0, Relay, SwitchShort, SwitchLong, SwitchDouble, Boot, Online, Offline
};
enum class CondType : uint8_t { None = 0, Channel, TimeBetween, AnyOn, AllOff };
enum class ActType : uint8_t { None = 0, Relay, Timer, AllOff, AllOn, Group };

struct Condition {
  CondType type = CondType::None;
  uint8_t  channel = 0;
  bool     state = false;
  int16_t  from = 0;  // minutes since midnight
  int16_t  to = 0;
};

struct ActionSpec {
  ActType  type = ActType::None;
  uint8_t  channelMask = 0;
  uint8_t  channel = 0;
  uint8_t  group = 0;
  Action   action = Action::Off;
  uint32_t seconds = 0;
  uint16_t delaySec = 0;
};

struct Rule {
  char        id[20];
  bool        enabled;
  TriggerType trigger;
  uint8_t     triggerChannel;   // 0xFF = any channel
  int8_t      triggerState;     // -1 = any, 0 = off, 1 = on
  Condition   conditions[AutomationEngine::kMaxConditions];
  uint8_t     condCount;
  ActionSpec  actions[AutomationEngine::kMaxActions];
  uint8_t     actionCount;
  uint16_t    cooldownSec;
  uint32_t    lastFiredMs;
};

/// A due action, queued so the trigger path never blocks and so delayed
/// actions do not need a task each.
struct PendingAction {
  ActionSpec spec;
  uint32_t   dueAtMs;
  uint8_t    depth;
};

Rule g_rules[AutomationEngine::kMaxRules];
uint8_t  g_count = 0;
uint32_t g_fired = 0;
uint32_t g_suppressed = 0;
TaskHandle_t g_task = nullptr;
QueueHandle_t g_queue = nullptr;

/// Depth of the currently-executing rule chain. An action performed at depth N
/// produces events that may only trigger rules up to kMaxChainDepth.
volatile uint8_t g_chainDepth = 0;

TriggerType triggerFrom(const char* s) {
  if (!s) return TriggerType::None;
  if (strcmp(s, "relay") == 0) return TriggerType::Relay;
  if (strcmp(s, "switchShort") == 0) return TriggerType::SwitchShort;
  if (strcmp(s, "switchLong") == 0) return TriggerType::SwitchLong;
  if (strcmp(s, "switchDouble") == 0) return TriggerType::SwitchDouble;
  if (strcmp(s, "boot") == 0) return TriggerType::Boot;
  if (strcmp(s, "online") == 0) return TriggerType::Online;
  if (strcmp(s, "offline") == 0) return TriggerType::Offline;
  return TriggerType::None;
}

const char* triggerName(TriggerType t) {
  switch (t) {
    case TriggerType::Relay:        return "relay";
    case TriggerType::SwitchShort:  return "switchShort";
    case TriggerType::SwitchLong:   return "switchLong";
    case TriggerType::SwitchDouble: return "switchDouble";
    case TriggerType::Boot:         return "boot";
    case TriggerType::Online:       return "online";
    case TriggerType::Offline:      return "offline";
    default:                        return "none";
  }
}

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

bool timeInWindow(int16_t from, int16_t to) {
  const int now = TimeService::minutesOfDay();
  if (now < 0) return false;  // no clock -> time conditions cannot be satisfied
  if (from <= to) return now >= from && now <= to;
  return now >= from || now <= to;  // window wraps past midnight
}

bool conditionsHold(const Rule& r) {
  for (uint8_t i = 0; i < r.condCount; ++i) {
    const Condition& c = r.conditions[i];
    switch (c.type) {
      case CondType::Channel:
        if (RelayManager::isOn(c.channel) != c.state) return false;
        break;
      case CondType::TimeBetween:
        if (!timeInWindow(c.from, c.to)) return false;
        break;
      case CondType::AnyOn:
        if (RelayManager::stateMask() == 0) return false;
        break;
      case CondType::AllOff:
        if (RelayManager::stateMask() != 0) return false;
        break;
      default:
        break;
    }
  }
  return true;
}

void runAction(const ActionSpec& a) {
  switch (a.type) {
    case ActType::Relay:
      RelayManager::commandMask(a.channelMask, a.action, Source::Automation);
      break;
    case ActType::Timer:
      RelayManager::setAutoOff(a.channel, a.seconds, Source::Automation);
      break;
    case ActType::AllOff:
      RelayManager::commandAll(Action::Off, Source::Automation);
      break;
    case ActType::AllOn:
      RelayManager::commandAll(Action::On, Source::Automation);
      break;
    case ActType::Group:
      RelayManager::commandGroup(a.group, a.action, Source::Automation);
      break;
    default:
      break;
  }
}

bool matchesTrigger(const Rule& r, const Event& e) {
  switch (r.trigger) {
    case TriggerType::Relay:
      if (e.type != EventType::RelayChanged) return false;
      if (r.triggerChannel != 0xFF && r.triggerChannel != e.channel) return false;
      if (r.triggerState >= 0 && (e.state ? 1 : 0) != r.triggerState) return false;
      return true;
    case TriggerType::SwitchShort:
      return e.type == EventType::SwitchShortPress &&
             (r.triggerChannel == 0xFF || r.triggerChannel == e.channel);
    case TriggerType::SwitchLong:
      return e.type == EventType::SwitchLongPress &&
             (r.triggerChannel == 0xFF || r.triggerChannel == e.channel);
    case TriggerType::SwitchDouble:
      return e.type == EventType::SwitchDoublePress &&
             (r.triggerChannel == 0xFF || r.triggerChannel == e.channel);
    case TriggerType::Online:
      return e.type == EventType::WifiConnected;
    case TriggerType::Offline:
      return e.type == EventType::WifiDisconnected;
    default:
      return false;
  }
}

void evaluate(const Event& e) {
  if (g_count == 0 || !g_queue) return;

  const uint8_t depth = g_chainDepth;
  if (depth >= AutomationEngine::kMaxChainDepth) {
    g_suppressed++;
    SH_LOGW(TAG, "chain depth limit reached, not evaluating further");
    return;
  }
  // An automation reacting to its own output is the classic runaway. Only
  // rules explicitly triggered by relay changes may chain at all.
  if (e.source == Source::Automation && e.type == EventType::RelayChanged &&
      depth + 1 >= AutomationEngine::kMaxChainDepth) {
    g_suppressed++;
    return;
  }

  const uint32_t now = millis();
  for (uint8_t i = 0; i < g_count; ++i) {
    Rule& r = g_rules[i];
    if (!r.enabled) continue;
    if (!matchesTrigger(r, e)) continue;

    if (r.cooldownSec > 0 && r.lastFiredMs != 0 &&
        static_cast<uint32_t>(now - r.lastFiredMs) < r.cooldownSec * 1000u) {
      g_suppressed++;
      SH_LOGD(TAG, "rule '%s' in cooldown", r.id);
      continue;
    }
    if (!conditionsHold(r)) continue;

    r.lastFiredMs = now;
    g_fired++;
    SH_LOGI(TAG, "rule '%s' fired on %s", r.id, triggerName(r.trigger));

    for (uint8_t a = 0; a < r.actionCount; ++a) {
      PendingAction pa{r.actions[a], now + r.actions[a].delaySec * 1000u,
                       static_cast<uint8_t>(depth + 1)};
      if (xQueueSend(g_queue, &pa, 0) != pdTRUE) {
        SH_LOGE(TAG, "action queue full, rule '%s' action %u dropped", r.id,
                static_cast<unsigned>(a));
      }
    }
  }
}

void onBusEvent(const Event& e, void* /*ctx*/) {
  switch (e.type) {
    case EventType::RelayChanged:
    case EventType::SwitchShortPress:
    case EventType::SwitchLongPress:
    case EventType::SwitchDoublePress:
    case EventType::WifiConnected:
    case EventType::WifiDisconnected:
      evaluate(e);
      break;
    default:
      break;
  }
}

}  // namespace

// ---------------------------------------------------------------------------

void AutomationEngine::begin() {
  memset(g_rules, 0, sizeof(g_rules));
  g_count = 0;
  g_queue = xQueueCreate(16, sizeof(PendingAction));

  char* buf = static_cast<char*>(malloc(kJsonCap));
  if (buf) {
    const size_t n = NvsStore::getString(nvsns::kAutomation, kKey, buf, kJsonCap, "");
    if (n > 0) {
      char err[80];
      if (!applyJson(buf, err, sizeof(err))) {
        SH_LOGE(TAG, "stored rules rejected: %s", err);
        g_count = 0;
      }
    }
    free(buf);
  }

  EventBus::subscribe(onBusEvent, nullptr, "automation");
  SH_LOGI(TAG, "%u rule(s) loaded", static_cast<unsigned>(g_count));
}

bool AutomationEngine::applyJson(const char* json, char* err, size_t errCap) {
  if (err && errCap) err[0] = '\0';
  JsonDocument doc;
  if (deserializeJson(doc, json ? json : "")) {
    if (err) snprintf(err, errCap, "invalid json");
    return false;
  }
  JsonArrayConst arr = doc.is<JsonArrayConst>() ? doc.as<JsonArrayConst>()
                                                : doc["automations"].as<JsonArrayConst>();
  if (arr.isNull()) {
    if (err) snprintf(err, errCap, "expected an array of rules");
    return false;
  }

  Rule staged[kMaxRules];
  memset(staged, 0, sizeof(staged));
  uint8_t n = 0;

  for (JsonObjectConst o : arr) {
    if (n >= kMaxRules) {
      if (err) snprintf(err, errCap, "too many rules (max %u)", kMaxRules);
      return false;
    }
    Rule& r = staged[n];

    const char* id = o["id"] | "";
    if (!id[0]) {
      if (err) snprintf(err, errCap, "rule %u has no id", n);
      return false;
    }
    strncpy(r.id, id, sizeof(r.id) - 1);
    r.enabled = o["enabled"] | true;
    r.cooldownSec = o["cooldownSec"] | 2;

    JsonObjectConst t = o["trigger"].as<JsonObjectConst>();
    r.trigger = triggerFrom(t["type"]);
    if (r.trigger == TriggerType::None) {
      if (err) snprintf(err, errCap, "rule '%s': unknown trigger type", r.id);
      return false;
    }
    r.triggerChannel = t["channel"].is<int>()
                           ? static_cast<uint8_t>(t["channel"].as<int>())
                           : 0xFF;
    if (r.triggerChannel != 0xFF && r.triggerChannel >= board::kChannelCount) {
      if (err) snprintf(err, errCap, "rule '%s': trigger channel out of range", r.id);
      return false;
    }
    r.triggerState = t["state"].is<bool>() ? (t["state"].as<bool>() ? 1 : 0) : -1;

    // conditions
    r.condCount = 0;
    for (JsonObjectConst c : o["conditions"].as<JsonArrayConst>()) {
      if (r.condCount >= kMaxConditions) {
        if (err) snprintf(err, errCap, "rule '%s': too many conditions", r.id);
        return false;
      }
      Condition& cd = r.conditions[r.condCount];
      const char* ct = c["type"] | "";
      if (strcmp(ct, "channel") == 0) {
        cd.type = CondType::Channel;
        cd.channel = static_cast<uint8_t>(c["channel"] | 0);
        cd.state = c["state"] | true;
        if (cd.channel >= board::kChannelCount) {
          if (err) snprintf(err, errCap, "rule '%s': condition channel out of range", r.id);
          return false;
        }
      } else if (strcmp(ct, "timeBetween") == 0) {
        cd.type = CondType::TimeBetween;
        cd.from = static_cast<int16_t>(c["from"] | 0);
        cd.to = static_cast<int16_t>(c["to"] | 0);
        if (cd.from < 0 || cd.from > 1439 || cd.to < 0 || cd.to > 1439) {
          if (err) snprintf(err, errCap, "rule '%s': time must be 0..1439", r.id);
          return false;
        }
      } else if (strcmp(ct, "anyOn") == 0) {
        cd.type = CondType::AnyOn;
      } else if (strcmp(ct, "allOff") == 0) {
        cd.type = CondType::AllOff;
      } else {
        if (err) snprintf(err, errCap, "rule '%s': unknown condition '%s'", r.id, ct);
        return false;
      }
      r.condCount++;
    }

    // actions
    r.actionCount = 0;
    for (JsonObjectConst a : o["actions"].as<JsonArrayConst>()) {
      if (r.actionCount >= kMaxActions) {
        if (err) snprintf(err, errCap, "rule '%s': too many actions", r.id);
        return false;
      }
      ActionSpec& as = r.actions[r.actionCount];
      const char* at = a["type"] | "";
      as.delaySec = static_cast<uint16_t>(a["delaySec"] | 0);
      if (as.delaySec > 3600) {
        if (err) snprintf(err, errCap, "rule '%s': delaySec too large", r.id);
        return false;
      }

      if (strcmp(at, "relay") == 0) {
        as.type = ActType::Relay;
        bool ok = false;
        as.action = actionFrom(a["action"], ok);
        if (!ok) {
          if (err) snprintf(err, errCap, "rule '%s': bad action", r.id);
          return false;
        }
        for (JsonVariantConst c : a["channels"].as<JsonArrayConst>()) {
          const int ch = c.as<int>();
          if (ch < 0 || ch >= board::kChannelCount) {
            if (err) snprintf(err, errCap, "rule '%s': action channel out of range", r.id);
            return false;
          }
          as.channelMask |= static_cast<uint8_t>(1u << ch);
        }
        if (as.channelMask == 0) {
          if (err) snprintf(err, errCap, "rule '%s': action has no channels", r.id);
          return false;
        }
      } else if (strcmp(at, "timer") == 0) {
        as.type = ActType::Timer;
        as.channel = static_cast<uint8_t>(a["channel"] | 0);
        as.seconds = a["seconds"] | 0u;
        if (as.channel >= board::kChannelCount || as.seconds == 0 ||
            as.seconds > 86400u) {
          if (err) snprintf(err, errCap, "rule '%s': invalid timer action", r.id);
          return false;
        }
      } else if (strcmp(at, "allOff") == 0) {
        as.type = ActType::AllOff;
      } else if (strcmp(at, "allOn") == 0) {
        as.type = ActType::AllOn;
      } else if (strcmp(at, "group") == 0) {
        as.type = ActType::Group;
        as.group = static_cast<uint8_t>(a["group"] | 0);
        bool ok = false;
        as.action = actionFrom(a["action"], ok);
        if (!ok || as.group == 0) {
          if (err) snprintf(err, errCap, "rule '%s': invalid group action", r.id);
          return false;
        }
      } else {
        if (err) snprintf(err, errCap, "rule '%s': unknown action '%s'", r.id, at);
        return false;
      }
      r.actionCount++;
    }

    if (r.actionCount == 0) {
      if (err) snprintf(err, errCap, "rule '%s': no actions", r.id);
      return false;
    }
    n++;
  }

  memcpy(g_rules, staged, sizeof(staged));
  g_count = n;
  SH_LOGI(TAG, "%u rule(s) applied", static_cast<unsigned>(n));
  return true;
}

size_t AutomationEngine::toJson(char* out, size_t cap) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < g_count; ++i) {
    const Rule& r = g_rules[i];
    JsonObject o = arr.add<JsonObject>();
    o["id"] = r.id;
    o["enabled"] = r.enabled;
    o["cooldownSec"] = r.cooldownSec;

    JsonObject t = o["trigger"].to<JsonObject>();
    t["type"] = triggerName(r.trigger);
    if (r.triggerChannel != 0xFF) t["channel"] = r.triggerChannel;
    if (r.triggerState >= 0) t["state"] = r.triggerState == 1;

    JsonArray conds = o["conditions"].to<JsonArray>();
    for (uint8_t c = 0; c < r.condCount; ++c) {
      const Condition& cd = r.conditions[c];
      JsonObject co = conds.add<JsonObject>();
      switch (cd.type) {
        case CondType::Channel:
          co["type"] = "channel";
          co["channel"] = cd.channel;
          co["state"] = cd.state;
          break;
        case CondType::TimeBetween:
          co["type"] = "timeBetween";
          co["from"] = cd.from;
          co["to"] = cd.to;
          break;
        case CondType::AnyOn:  co["type"] = "anyOn"; break;
        case CondType::AllOff: co["type"] = "allOff"; break;
        default: break;
      }
    }

    JsonArray acts = o["actions"].to<JsonArray>();
    for (uint8_t a = 0; a < r.actionCount; ++a) {
      const ActionSpec& as = r.actions[a];
      JsonObject ao = acts.add<JsonObject>();
      if (as.delaySec) ao["delaySec"] = as.delaySec;
      switch (as.type) {
        case ActType::Relay: {
          ao["type"] = "relay";
          ao["action"] = actionName(as.action);
          JsonArray ch = ao["channels"].to<JsonArray>();
          for (uint8_t c = 0; c < board::kChannelCount; ++c) {
            if (as.channelMask & (1u << c)) ch.add(c);
          }
          break;
        }
        case ActType::Timer:
          ao["type"] = "timer";
          ao["channel"] = as.channel;
          ao["seconds"] = as.seconds;
          break;
        case ActType::AllOff: ao["type"] = "allOff"; break;
        case ActType::AllOn:  ao["type"] = "allOn"; break;
        case ActType::Group:
          ao["type"] = "group";
          ao["group"] = as.group;
          ao["action"] = actionName(as.action);
          break;
        default: break;
      }
    }
  }
  return serializeJson(doc, out, cap);
}

bool AutomationEngine::save() {
  char* buf = static_cast<char*>(malloc(kJsonCap));
  if (!buf) return false;
  const size_t n = toJson(buf, kJsonCap);
  const bool ok = n > 0 && n < kJsonCap &&
                  NvsStore::putString(nvsns::kAutomation, kKey, buf);
  free(buf);
  return ok;
}

uint8_t AutomationEngine::ruleCount() { return g_count; }
uint32_t AutomationEngine::firedCount() { return g_fired; }
uint32_t AutomationEngine::suppressedCount() { return g_suppressed; }

void AutomationEngine::startTask() {
  if (g_task) return;
  xTaskCreatePinnedToCore(&AutomationEngine::taskEntry, "auto", 4096, nullptr, 2,
                          &g_task, 1);
}

void AutomationEngine::taskEntry(void* /*arg*/) {
  esp_task_wdt_add(nullptr);

  // Let the rest of the system settle before boot rules run - firing them
  // while Wi-Fi is still associating would make "online" conditions flap.
  vTaskDelay(pdMS_TO_TICKS(3000));

  for (uint8_t i = 0; i < g_count; ++i) {
    Rule& r = g_rules[i];
    if (r.enabled && r.trigger == TriggerType::Boot && conditionsHold(r)) {
      SH_LOGI(TAG, "rule '%s' fired on boot", r.id);
      g_fired++;
      for (uint8_t a = 0; a < r.actionCount; ++a) {
        PendingAction pa{r.actions[a], millis() + r.actions[a].delaySec * 1000u, 1};
        xQueueSend(g_queue, &pa, 0);
      }
    }
  }

  // Deferred actions waiting on their delay.
  PendingAction deferred[8];
  uint8_t deferredCount = 0;

  for (;;) {
    esp_task_wdt_reset();

    PendingAction pa{};
    while (xQueueReceive(g_queue, &pa, 0) == pdTRUE) {
      if (static_cast<int32_t>(millis() - pa.dueAtMs) >= 0) {
        g_chainDepth = pa.depth;
        runAction(pa.spec);
        g_chainDepth = 0;
      } else if (deferredCount < 8) {
        deferred[deferredCount++] = pa;
      } else {
        SH_LOGW(TAG, "deferred action table full, dropping one");
      }
    }

    for (uint8_t i = 0; i < deferredCount;) {
      if (static_cast<int32_t>(millis() - deferred[i].dueAtMs) >= 0) {
        g_chainDepth = deferred[i].depth;
        runAction(deferred[i].spec);
        g_chainDepth = 0;
        deferred[i] = deferred[--deferredCount];
      } else {
        ++i;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

}  // namespace sh
