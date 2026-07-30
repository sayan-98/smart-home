#include "switchio/SwitchScanner.h"

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config/Board.h"
#include "config/ConfigStore.h"
#include "core/EventBus.h"
#include "log/Logger.h"
#include "relay/RelayManager.h"
#include "util/Debouncer.h"
#include "util/PressDecoder.h"

namespace sh {
namespace {

constexpr const char* TAG = "switch";
constexpr uint8_t kN = board::kChannelCount;

/// Idle wake period. Gestures and the factory-reset hold need a tick even when
/// no edge arrives; edges themselves wake the task immediately via notify.
constexpr uint32_t kTickMs = 10;

TaskHandle_t g_task = nullptr;

Debouncer    g_debounce[kN];
PressDecoder g_press[kN];
bool         g_asserted[kN] = {false};

uint32_t g_resetHeldSinceMs = 0;
bool     g_resetPending = false;

/// Reads the pin and converts it to "contact closed", honouring both the
/// board-level polarity and the per-channel inversion.
bool readAsserted(uint8_t ch) {
  const int level = digitalRead(board::kSwitchPins[ch]);
  bool a = board::kSwitchActiveLow ? (level == LOW) : (level == HIGH);
  if (ConfigStore::channel(ch).switchInverted) a = !a;
  return a;
}

void IRAM_ATTR onEdgeIsr(void* /*arg*/) {
  // Do as little as possible here: just wake the scanner task, which re-reads
  // every pin. Cheaper and more robust than queueing per-pin events, and it
  // cannot overflow under contact bounce.
  BaseType_t higherPriorityWoken = pdFALSE;
  if (g_task) {
    vTaskNotifyGiveFromISR(g_task, &higherPriorityWoken);
  }
  if (higherPriorityWoken == pdTRUE) portYIELD_FROM_ISR();
}

void runGesture(uint8_t ch, GestureAction action) {
  const ChannelConfig& cfg = ConfigStore::channel(ch);
  switch (action) {
    case GestureAction::ToggleOwn:
      RelayManager::command(ch, Action::Toggle, Source::Physical);
      break;
    case GestureAction::AllOff:
      RelayManager::commandAll(Action::Off, Source::Physical);
      break;
    case GestureAction::AllOn:
      RelayManager::commandAll(Action::On, Source::Physical);
      break;
    case GestureAction::GroupToggle:
      if (cfg.group != 0) {
        RelayManager::commandGroup(cfg.group, Action::Toggle, Source::Physical);
      } else {
        RelayManager::command(ch, Action::Toggle, Source::Physical);
      }
      break;
    default:
      break;
  }
}

void publishSwitchEvent(EventType type, uint8_t ch, int32_t value) {
  Event e;
  e.type = type;
  e.channel = ch;
  e.source = Source::Physical;
  e.value = value;
  EventBus::publish(e);
}

void handleAcceptedEdge(uint8_t ch, bool assertedNow, uint32_t now) {
  const ChannelConfig& cfg = ConfigStore::channel(ch);
  g_asserted[ch] = assertedNow;

  if (cfg.switchMode == SwitchMode::Latching) {
    // Any position change toggles the RELAY, not "match the switch position".
    // This is the desync fix - see the header comment.
    SH_LOGD(TAG, "ch%u latching flip -> toggle", static_cast<unsigned>(ch));
    publishSwitchEvent(EventType::SwitchShortPress, ch, 0);
    RelayManager::command(ch, Action::Toggle, Source::Physical);
    return;
  }

  const PressResult r = g_press[ch].onEdge(assertedNow, now);
  switch (r) {
    case PressResult::Short:
      publishSwitchEvent(EventType::SwitchShortPress, ch, 0);
      RelayManager::command(ch, Action::Toggle, Source::Physical);
      break;
    case PressResult::Double:
      SH_LOGI(TAG, "ch%u double press", static_cast<unsigned>(ch));
      publishSwitchEvent(EventType::SwitchDoublePress, ch, 0);
      runGesture(ch, cfg.doublePressAction);
      break;
    case PressResult::Long:  // only reachable from tick(), listed for clarity
    case PressResult::None:
    default:
      break;
  }
}

void tickGestures(uint32_t now) {
  for (uint8_t ch = 0; ch < kN; ++ch) {
    const ChannelConfig& cfg = ConfigStore::channel(ch);
    if (!cfg.switchEnabled || cfg.switchMode != SwitchMode::Momentary) continue;

    const PressResult r = g_press[ch].tick(now);
    if (r == PressResult::Long) {
      SH_LOGI(TAG, "ch%u long press (%u ms)", static_cast<unsigned>(ch),
              static_cast<unsigned>(cfg.longPressMs));
      publishSwitchEvent(EventType::SwitchLongPress, ch, cfg.longPressMs);
      runGesture(ch, cfg.longPressAction);
    } else if (r == PressResult::Short) {
      publishSwitchEvent(EventType::SwitchShortPress, ch, 0);
      RelayManager::command(ch, Action::Toggle, Source::Physical);
    }
  }
}

void tickFactoryReset(uint32_t now) {
  if (board::kFactoryResetPin < 0) return;
  const bool held = digitalRead(board::kFactoryResetPin) == LOW;  // BOOT is active low

  if (!held) {
    if (g_resetPending) SH_LOGI(TAG, "factory reset aborted");
    g_resetHeldSinceMs = 0;
    g_resetPending = false;
    return;
  }

  if (g_resetHeldSinceMs == 0) {
    g_resetHeldSinceMs = now;
    g_resetPending = true;
    SH_LOGW(TAG, "factory reset: hold BOOT for %u s to confirm",
            static_cast<unsigned>(board::kFactoryResetHoldMs / 1000));
    return;
  }

  const uint32_t heldMs = now - g_resetHeldSinceMs;
  if (heldMs >= board::kFactoryResetHoldMs) {
    SH_LOGE(TAG, "FACTORY RESET triggered from BOOT button");
    g_resetHeldSinceMs = 0;
    g_resetPending = false;
    Event e;
    e.type = EventType::FactoryReset;
    e.source = Source::Physical;
    EventBus::publish(e);
  }
}

}  // namespace

void SwitchScanner::begin() {
  const uint32_t now = millis();
  for (uint8_t ch = 0; ch < kN; ++ch) {
    pinMode(board::kSwitchPins[ch], INPUT_PULLUP);
  }
  // Let the pull-ups settle before priming, otherwise the first read can catch
  // a still-rising line and we would prime to the wrong level.
  delay(5);

  for (uint8_t ch = 0; ch < kN; ++ch) {
    const ChannelConfig& cfg = ConfigStore::channel(ch);
    const bool a = readAsserted(ch);
    g_asserted[ch] = a;
    // Prime with the raw pin level (not the asserted interpretation) because
    // that is what update() will be fed.
    g_debounce[ch].configure(cfg.debounceMs, a);
    g_debounce[ch].update(a, now);  // primes without reporting a change
    g_press[ch].configure(cfg.longPressMs, cfg.doublePressMs);
  }

  if (board::kFactoryResetPin >= 0) {
    pinMode(board::kFactoryResetPin, INPUT_PULLUP);
  }

  uint8_t initialMask = 0;
  for (uint8_t i = 0; i < kN; ++i) {
    if (g_asserted[i]) initialMask |= (1u << i);
  }
  SH_LOGI(TAG, "init: %u inputs, initial mask=0x%02X", static_cast<unsigned>(kN),
          static_cast<unsigned>(initialMask));
}

void SwitchScanner::startTask() {
  if (g_task) return;
  xTaskCreatePinnedToCore(&SwitchScanner::taskEntry, "switch", 3072, nullptr, 5,
                          &g_task, 1);

  // Interrupts are attached only after the task exists, so the ISR always has
  // a valid handle to notify.
  for (uint8_t ch = 0; ch < kN; ++ch) {
    attachInterruptArg(digitalPinToInterrupt(board::kSwitchPins[ch]), onEdgeIsr,
                       nullptr, CHANGE);
  }
}

void SwitchScanner::reconfigure() {
  const uint32_t now = millis();
  for (uint8_t ch = 0; ch < kN; ++ch) {
    const ChannelConfig& cfg = ConfigStore::channel(ch);
    const bool a = readAsserted(ch);
    g_debounce[ch].configure(cfg.debounceMs, a);
    g_debounce[ch].update(a, now);
    g_press[ch].configure(cfg.longPressMs, cfg.doublePressMs);
    g_asserted[ch] = a;
  }
  SH_LOGI(TAG, "reconfigured from settings");
}

void SwitchScanner::taskEntry(void* /*arg*/) {
  esp_task_wdt_add(nullptr);
  for (;;) {
    // Wake on an edge, or every kTickMs for gestures and the reset hold.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kTickMs));

    const uint32_t now = millis();
    for (uint8_t ch = 0; ch < kN; ++ch) {
      const ChannelConfig& cfg = ConfigStore::channel(ch);
      if (!cfg.switchEnabled) continue;

      const bool raw = readAsserted(ch);
      if (g_debounce[ch].update(raw, now)) {
        handleAcceptedEdge(ch, raw, now);
      }
    }

    tickGestures(now);
    tickFactoryReset(now);
    esp_task_wdt_reset();
  }
}

bool SwitchScanner::asserted(uint8_t channel) {
  return channel < kN && g_asserted[channel];
}

bool SwitchScanner::factoryResetPending() { return g_resetPending; }

uint32_t SwitchScanner::factoryResetHeldMs() {
  return g_resetHeldSinceMs ? (millis() - g_resetHeldSinceMs) : 0;
}

}  // namespace sh
