// ---------------------------------------------------------------------------
//  Diagnostics.h - the 30 s heartbeat and the health signals behind it.
//
//  The heartbeat is not decoration. Three specific failures on this hardware
//  are invisible without it and get misdiagnosed as "the firmware is buggy":
//    * brownout resets from an undersized 5 V supply or the JD-VCC jumper,
//    * a slow heap leak that only bites after days of uptime,
//    * Wi-Fi that is technically associated but at -85 dBm and dropping.
//  All three are reported here, with the reset reason, so the cause is
//  visible in the app rather than inferred.
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sh {

class Diagnostics {
 public:
  static constexpr uint32_t kHeartbeatMs = 30000;

  static void begin();
  static void startTask();

  static size_t toJson(char* out, size_t cap);

  static uint32_t heartbeatCount();
  static uint32_t lowHeapEvents();
  static uint32_t minHeapSeen();

  /// True when free heap has fallen below the danger threshold - the app
  /// surfaces this before the device starts refusing TLS handshakes.
  static bool heapCritical();

 private:
  static void taskEntry(void* arg);
};

}  // namespace sh
