#include "relay/RelayManager.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <driver/gpio.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

#include "config/ConfigStore.h"
#include "core/EventBus.h"
#include "log/Logger.h"
#include "net/TimeService.h"
#include "storage/NvsStore.h"
#include "util/Crc32.h"

namespace sh {
namespace {

constexpr const char* TAG = "relay";
constexpr uint8_t kN = board::kChannelCount;
constexpr const char* kPersistKey = "st";
constexpr uint32_t kPersistMagic = 0x5348524Cu;  // 'SHRL'
constexpr uint16_t kPersistVersion = 1;

/// Bumped on every boot so a revision can never go backwards even if the last
/// pre-power-loss writes never reached flash.
constexpr uint32_t kRevBootMargin = 64;

struct __attribute__((packed)) PersistedState {
  uint32_t magic;
  uint16_t version;
  uint8_t  states;      // bit i = channel i is ON
  uint8_t  reserved;
  uint32_t revCounter;
  uint32_t crc;         // over everything above
};

struct Cmd {
  uint8_t  channel;   // 0xFE = mask op
  uint8_t  mask;      // used when channel == 0xFE
  Action   action;
  Source   source;
  uint32_t rev;
  bool     hasRev;
};

QueueHandle_t g_queue = nullptr;
TaskHandle_t  g_task = nullptr;

ChannelState g_state[kN];
uint32_t g_globalRev = 0;
uint32_t g_lastChangeMs[kN] = {0};

bool     g_dirty = false;
uint32_t g_dirtyAtMs = 0;
uint8_t  g_gangDepth = 0;

inline uint8_t activeLevel() { return board::kRelayActiveLow ? LOW : HIGH; }
inline uint8_t inactiveLevel() { return board::kRelayActiveLow ? HIGH : LOW; }

void writePin(uint8_t ch, bool on) {
  digitalWrite(board::kRelayPins[ch], on ? activeLevel() : inactiveLevel());
}

// --- persistence ----------------------------------------------------------

void persistNow() {
  PersistedState p{};
  p.magic = kPersistMagic;
  p.version = kPersistVersion;
  p.states = 0;
  for (uint8_t i = 0; i < kN; ++i) {
    if (g_state[i].on) p.states |= (1u << i);
  }
  p.revCounter = g_globalRev;
  p.crc = crc32(&p, sizeof(PersistedState) - sizeof(uint32_t));

  if (NvsStore::putBlob(nvsns::kRelay, kPersistKey, &p, sizeof(p))) {
    g_dirty = false;
    SH_LOGD(TAG, "state persisted mask=0x%02X rev=%lu",
            static_cast<unsigned>(p.states),
            static_cast<unsigned long>(p.revCounter));
  } else {
    SH_LOGE(TAG, "failed to persist relay state");
  }
}

bool loadPersisted(PersistedState& out) {
  PersistedState p{};
  const size_t n = NvsStore::getBlob(nvsns::kRelay, kPersistKey, &p, sizeof(p));
  if (n != sizeof(p)) return false;
  if (p.magic != kPersistMagic || p.version != kPersistVersion) return false;
  const uint32_t expect = crc32(&p, sizeof(PersistedState) - sizeof(uint32_t));
  if (expect != p.crc) {
    SH_LOGW(TAG, "persisted state CRC mismatch, ignoring");
    return false;
  }
  out = p;
  return true;
}

// --- core apply -----------------------------------------------------------

bool resolveTarget(uint8_t ch, Action action) {
  switch (action) {
    case Action::On:   return true;
    case Action::Off:  return false;
    default:           return !g_state[ch].on;
  }
}

/// Returns true if the relay actually changed.
bool applyOne(uint8_t ch, Action action, Source source, bool hasRev, uint32_t rev) {
  if (ch >= kN) return false;

  const ChannelConfig& cfg = ConfigStore::channel(ch);
  if (!cfg.relayEnabled) {
    SH_LOGD(TAG, "ch%u disabled, command ignored", static_cast<unsigned>(ch));
    return false;
  }

  // Stale-cloud guard: a command carrying an explicit revision only wins if it
  // is newer than what the channel already has. A wall switch press that
  // happened after the cloud message was sent must not be undone by it.
  if (hasRev && rev <= g_state[ch].rev) {
    SH_LOGD(TAG, "ch%u stale rev %lu <= %lu, ignored", static_cast<unsigned>(ch),
            static_cast<unsigned long>(rev),
            static_cast<unsigned long>(g_state[ch].rev));
    Event e;
    e.type = EventType::RelayCommandRejected;
    e.channel = ch;
    e.source = source;
    e.rev = g_state[ch].rev;
    e.value = 1;  // reason: stale revision
    EventBus::publish(e);
    return false;
  }

  const bool target = resolveTarget(ch, action);
  if (target == g_state[ch].on) return false;  // already there, nothing to do

  // Contact-protection rate limit. A human cannot hit this; a runaway
  // automation can, and would chatter the coil until the contacts weld.
  // Physical presses bypass it - the person in the room always wins.
  const uint32_t now = millis();
  if (source != Source::Physical && source != Source::Restore &&
      static_cast<uint32_t>(now - g_lastChangeMs[ch]) < board::kRelayMinIntervalMs) {
    SH_LOGW(TAG, "ch%u rate limited (%lu ms since last change)",
            static_cast<unsigned>(ch),
            static_cast<unsigned long>(now - g_lastChangeMs[ch]));
    Event e;
    e.type = EventType::RelayCommandRejected;
    e.channel = ch;
    e.source = source;
    e.rev = g_state[ch].rev;
    e.value = 2;  // reason: rate limited
    EventBus::publish(e);
    return false;
  }

  writePin(ch, target);

  g_globalRev++;
  g_state[ch].on = target;
  g_state[ch].source = source;
  g_state[ch].rev = g_globalRev;
  g_state[ch].tsSynced = TimeService::isSynced();
  g_state[ch].changedAtMs = TimeService::nowMsOrUptime();
  g_lastChangeMs[ch] = now;

  // Auto-off timer: armed on turn-on, cleared on turn-off.
  if (target && cfg.autoOffSec > 0) {
    g_state[ch].autoOffAtMs = now + cfg.autoOffSec * 1000u;
  } else {
    g_state[ch].autoOffAtMs = 0;
  }

  if (!g_dirty) {
    g_dirty = true;
    g_dirtyAtMs = now;
  }

  SH_LOGI(TAG, "ch%u -> %s (%s, rev %lu)", static_cast<unsigned>(ch),
          target ? "ON" : "OFF", toString(source),
          static_cast<unsigned long>(g_state[ch].rev));

  EventBus::publishRelay(ch, target, source, g_state[ch].rev,
                         g_state[ch].changedAtMs, g_state[ch].tsSynced);
  return true;
}

void handleCmd(const Cmd& c);

/// Services the command queue for `ms` without applying a relay change.
/// Used between gang stagger steps so a wall switch press is still handled
/// within a few milliseconds even while an all-off is rippling through.
void drainFor(uint32_t ms) {
  const uint32_t end = millis() + ms;
  for (;;) {
    const int32_t remain = static_cast<int32_t>(end - millis());
    if (remain <= 0) return;
    Cmd c{};
    if (xQueueReceive(g_queue, &c, pdMS_TO_TICKS(remain)) == pdTRUE) {
      handleCmd(c);
    }
  }
}

void applyMask(uint8_t mask, Action action, Source source) {
  // Depth guard: a gang command arriving during another gang's stagger is
  // applied without its own stagger rather than nesting further.
  const bool stagger = (g_gangDepth == 0);
  g_gangDepth++;

  uint8_t remaining = mask;
  for (uint8_t ch = 0; ch < kN; ++ch) {
    if (!(mask & (1u << ch))) continue;
    applyOne(ch, action, source, false, 0);
    remaining = static_cast<uint8_t>(remaining & ~(1u << ch));
    if (stagger && remaining != 0) drainFor(board::kRelayGangStaggerMs);
  }

  g_gangDepth--;
}

void handleCmd(const Cmd& c) {
  if (c.channel == 0xFE) {
    applyMask(c.mask, c.action, c.source);
  } else {
    applyOne(c.channel, c.action, c.source, c.hasRev, c.rev);
  }
}

bool enqueue(const Cmd& c) {
  if (!g_queue) return false;
  // Never block: the callers include the switch task, whose latency budget is
  // the whole point of this design.
  if (xQueueSend(g_queue, &c, 0) != pdTRUE) {
    SH_LOGE(TAG, "command queue full, dropped ch=%u", static_cast<unsigned>(c.channel));
    return false;
  }
  return true;
}

void tickTimers() {
  const uint32_t now = millis();
  for (uint8_t ch = 0; ch < kN; ++ch) {
    if (g_state[ch].autoOffAtMs == 0) continue;
    if (static_cast<int32_t>(now - g_state[ch].autoOffAtMs) >= 0) {
      g_state[ch].autoOffAtMs = 0;
      SH_LOGI(TAG, "ch%u auto-off timer expired", static_cast<unsigned>(ch));
      applyOne(ch, Action::Off, Source::Timer, false, 0);
    }
  }
}

void tickPersist() {
  if (!g_dirty) return;
  if (static_cast<uint32_t>(millis() - g_dirtyAtMs) < board::kRelayPersistDebounceMs) return;
  persistNow();
}

}  // namespace

// ---------------------------------------------------------------------------

void RelayManager::begin() {
  // Bring each relay pin up without ever driving the active level.
  //
  // The order matters and is not obvious. On arduino-esp32 3.x, digitalWrite()
  // on a pin that has not been claimed as a GPIO is a no-op that logs an error,
  // so the naive "write the latch, then pinMode(OUTPUT)" trick silently does
  // nothing. Instead:
  //   1. pinMode(INPUT_PULLUP) claims the pin and, for an active-LOW board,
  //      the weak pull-up already holds the line at "relay off".
  //   2. gpio_set_level() preloads the output latch while the driver is still
  //      disabled. This is the step that actually prevents the glitch.
  //   3. pinMode(OUTPUT) enables the driver, which now emits the correct level
  //      from its first instant. It does not clear the latch, because the pin
  //      is already registered as a GPIO from step 1.
  //
  // The real protection during reset is still the external pull-ups
  // (docs/WIRING.md) - the code is not running then. This just makes sure the
  // firmware does not add a glitch of its own once it is.
  for (uint8_t i = 0; i < kN; ++i) {
    const int pin = board::kRelayPins[i];
    const gpio_num_t gpio = static_cast<gpio_num_t>(pin);

    pinMode(pin, INPUT_PULLUP);
    gpio_set_level(gpio, inactiveLevel());
    pinMode(pin, OUTPUT);
    digitalWrite(pin, inactiveLevel());

    g_state[i] = ChannelState{};
    g_state[i].source = Source::Restore;
    g_lastChangeMs[i] = 0;
  }

  PersistedState p{};
  const bool havePersisted = loadPersisted(p);
  g_globalRev = (havePersisted ? p.revCounter : 0) + kRevBootMargin;

  if (!g_queue) g_queue = xQueueCreate(32, sizeof(Cmd));

  // Restore. Deliberately direct rather than through the queue: the task is
  // not running yet, and the coils should reach their intended state before
  // anything else in boot can take time.
  for (uint8_t i = 0; i < kN; ++i) {
    const ChannelConfig& cfg = ConfigStore::channel(i);
    bool target = false;
    switch (cfg.restore) {
      case RestoreMode::On:   target = true; break;
      case RestoreMode::Last: target = havePersisted && (p.states & (1u << i)); break;
      default:                target = false; break;
    }
    if (!cfg.relayEnabled) target = false;

    if (target) {
      writePin(i, true);
      g_globalRev++;
      g_state[i].on = true;
      g_state[i].rev = g_globalRev;
      g_state[i].source = Source::Restore;
      g_state[i].changedAtMs = 0;
      if (cfg.autoOffSec > 0) g_state[i].autoOffAtMs = millis() + cfg.autoOffSec * 1000u;
    }
  }

  SH_LOGI(TAG, "init: %u channels, restored mask=0x%02X, rev base %lu%s",
          static_cast<unsigned>(kN), static_cast<unsigned>(stateMask()),
          static_cast<unsigned long>(g_globalRev),
          havePersisted ? "" : " (no stored state)");

  g_dirty = true;
  g_dirtyAtMs = millis();
}

void RelayManager::startTask() {
  if (g_task) return;
  xTaskCreatePinnedToCore(&RelayManager::taskEntry, "relay", 3072, nullptr, 4,
                          &g_task, 1);
}

void RelayManager::taskEntry(void* /*arg*/) {
  esp_task_wdt_add(nullptr);
  for (;;) {
    Cmd c{};
    if (xQueueReceive(g_queue, &c, pdMS_TO_TICKS(20)) == pdTRUE) {
      handleCmd(c);
    }
    tickTimers();
    tickPersist();
    esp_task_wdt_reset();
  }
}

bool RelayManager::command(uint8_t channel, Action action, Source source) {
  Cmd c{channel, 0, action, source, 0, false};
  return enqueue(c);
}

bool RelayManager::commandWithRev(uint8_t channel, Action action, Source source,
                                  uint32_t rev) {
  Cmd c{channel, 0, action, source, rev, true};
  return enqueue(c);
}

bool RelayManager::commandMask(uint8_t mask, Action action, Source source) {
  if (mask == 0) return false;
  Cmd c{0xFE, mask, action, source, 0, false};
  return enqueue(c);
}

bool RelayManager::commandAll(Action action, Source source) {
  const uint8_t full = (kN >= 8) ? 0xFF : static_cast<uint8_t>((1u << kN) - 1u);
  return commandMask(full, action, source);
}

bool RelayManager::commandGroup(uint8_t group, Action action, Source source) {
  if (group == 0) return false;
  uint8_t mask = 0;
  for (uint8_t i = 0; i < kN; ++i) {
    if (ConfigStore::channel(i).group == group) mask |= (1u << i);
  }
  if (mask == 0) {
    SH_LOGW(TAG, "group %u has no channels", static_cast<unsigned>(group));
    return false;
  }
  return commandMask(mask, action, source);
}

bool RelayManager::setAutoOff(uint8_t channel, uint32_t seconds, Source source) {
  if (channel >= kN) return false;
  if (seconds == 0) {
    g_state[channel].autoOffAtMs = 0;
    SH_LOGI(TAG, "ch%u auto-off cancelled", static_cast<unsigned>(channel));
    return true;
  }
  if (seconds > 86400u * 7u) return false;
  // Arm first so the timer survives even if the relay was already on.
  g_state[channel].autoOffAtMs = millis() + seconds * 1000u;
  command(channel, Action::On, source);
  SH_LOGI(TAG, "ch%u on for %lu s (%s)", static_cast<unsigned>(channel),
          static_cast<unsigned long>(seconds), toString(source));
  return true;
}

ChannelState RelayManager::state(uint8_t channel) {
  if (channel >= kN) return ChannelState{};
  return g_state[channel];
}

bool RelayManager::isOn(uint8_t channel) {
  return channel < kN && g_state[channel].on;
}

uint8_t RelayManager::stateMask() {
  uint8_t m = 0;
  for (uint8_t i = 0; i < kN; ++i) {
    if (g_state[i].on) m |= (1u << i);
  }
  return m;
}

uint32_t RelayManager::revision(uint8_t channel) {
  return channel < kN ? g_state[channel].rev : 0;
}

uint32_t RelayManager::globalRevision() { return g_globalRev; }

size_t RelayManager::channelToJson(uint8_t channel, char* out, size_t cap) {
  if (channel >= kN) return 0;
  JsonDocument doc;
  const ChannelState& s = g_state[channel];
  const ChannelConfig& c = ConfigStore::channel(channel);
  doc["channel"] = channel;
  doc["name"] = c.name;
  doc["icon"] = c.icon;
  doc["state"] = s.on;
  doc["source"] = toString(s.source);
  doc["rev"] = s.rev;
  doc["changedAt"] = s.changedAtMs;
  doc["tsSynced"] = s.tsSynced;
  doc["group"] = c.group;
  doc["enabled"] = c.relayEnabled;
  const uint32_t now = millis();
  doc["autoOffInSec"] =
      s.autoOffAtMs && static_cast<int32_t>(s.autoOffAtMs - now) > 0
          ? (s.autoOffAtMs - now) / 1000u
          : 0;
  return serializeJson(doc, out, cap);
}

size_t RelayManager::toJson(char* out, size_t cap) {
  JsonDocument doc;
  doc["rev"] = g_globalRev;
  doc["mask"] = stateMask();
  JsonArray arr = doc["channels"].to<JsonArray>();
  const uint32_t now = millis();
  for (uint8_t i = 0; i < kN; ++i) {
    const ChannelState& s = g_state[i];
    const ChannelConfig& c = ConfigStore::channel(i);
    JsonObject o = arr.add<JsonObject>();
    o["channel"] = i;
    o["name"] = c.name;
    o["icon"] = c.icon;
    o["state"] = s.on;
    o["source"] = toString(s.source);
    o["rev"] = s.rev;
    o["changedAt"] = s.changedAtMs;
    o["tsSynced"] = s.tsSynced;
    o["group"] = c.group;
    o["enabled"] = c.relayEnabled;
    o["autoOffInSec"] =
        s.autoOffAtMs && static_cast<int32_t>(s.autoOffAtMs - now) > 0
            ? (s.autoOffAtMs - now) / 1000u
            : 0;
  }
  return serializeJson(doc, out, cap);
}

void RelayManager::flush() {
  if (g_dirty) persistNow();
}

void RelayManager::safeShutdown(Source source) {
  SH_LOGW(TAG, "safe shutdown, all channels off (%s)", toString(source));
  for (uint8_t i = 0; i < kN; ++i) {
    writePin(i, false);
    g_state[i].on = false;
    g_state[i].autoOffAtMs = 0;
    g_state[i].source = source;
  }
}

}  // namespace sh
