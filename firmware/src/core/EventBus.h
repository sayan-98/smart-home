// ---------------------------------------------------------------------------
//  EventBus.h - in-process publish/subscribe.
//
//  Dispatch is SYNCHRONOUS: publish() runs every handler on the calling task.
//  That is intentional - it keeps ordering strict and avoids a second queue
//  hop for the latency-critical relay path.
//
//  THE HANDLER CONTRACT: a handler must never block. It enqueues onto its own
//  service queue and returns. A handler that does network I/O inline will
//  stall the RelayManager task and break the <20 ms switch->relay guarantee.
//
//  publish() is safe from any task. NOT safe from an ISR.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Types.h"

namespace sh {

class EventBus {
 public:
  using Handler = void (*)(const Event& event, void* ctx);

  static void begin();

  /// Returns false if the handler table is full (compile-time bound).
  static bool subscribe(Handler handler, void* ctx, const char* name);

  static void publish(const Event& event);

  /// Convenience for the common relay case.
  static void publishRelay(uint8_t channel, bool state, Source source,
                           uint32_t rev, uint64_t tsMs, bool tsSynced);

  static uint8_t handlerCount();

 private:
  static constexpr uint8_t kMaxHandlers = 12;
};

}  // namespace sh
