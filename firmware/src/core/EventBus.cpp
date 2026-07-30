#include "core/EventBus.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "log/Logger.h"

namespace sh {
namespace {

constexpr const char* TAG = "bus";

struct Subscription {
  EventBus::Handler handler = nullptr;
  void* ctx = nullptr;
  const char* name = nullptr;
};

Subscription g_subs[12];
uint8_t g_count = 0;
SemaphoreHandle_t g_mutex = nullptr;

}  // namespace

void EventBus::begin() {
  if (!g_mutex) g_mutex = xSemaphoreCreateRecursiveMutex();
  g_count = 0;
}

bool EventBus::subscribe(Handler handler, void* ctx, const char* name) {
  if (!handler) return false;
  if (g_mutex) xSemaphoreTakeRecursive(g_mutex, portMAX_DELAY);
  bool ok = false;
  if (g_count < kMaxHandlers) {
    g_subs[g_count] = {handler, ctx, name};
    g_count++;
    ok = true;
  }
  if (g_mutex) xSemaphoreGiveRecursive(g_mutex);
  if (!ok) SH_LOGE(TAG, "handler table full, '%s' dropped", name ? name : "?");
  return ok;
}

void EventBus::publish(const Event& event) {
  if (!g_mutex) return;
  // Recursive: a handler is allowed to publish a follow-up event.
  if (xSemaphoreTakeRecursive(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    SH_LOGE(TAG, "publish timeout, event %u dropped",
            static_cast<unsigned>(event.type));
    return;
  }
  const uint8_t count = g_count;
  for (uint8_t i = 0; i < count; ++i) {
    if (g_subs[i].handler) g_subs[i].handler(event, g_subs[i].ctx);
  }
  xSemaphoreGiveRecursive(g_mutex);
}

void EventBus::publishRelay(uint8_t channel, bool state, Source source,
                            uint32_t rev, uint64_t tsMs, bool tsSynced) {
  Event e;
  e.type = EventType::RelayChanged;
  e.channel = channel;
  e.state = state;
  e.source = source;
  e.rev = rev;
  e.tsMs = tsMs;
  e.tsSynced = tsSynced;
  publish(e);
}

uint8_t EventBus::handlerCount() { return g_count; }

}  // namespace sh
