// ---------------------------------------------------------------------------
//  DeviceInfo.h - stable identity and the capability advertisement sent to the
//  backend on registration.
//
//  The UUID is derived deterministically from the eFuse MAC, so it survives a
//  factory reset and a reflash. That matters: a device that changes identity
//  every wipe would orphan its rows in the backend and could be re-claimed by
//  someone else.
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sh {

class DeviceInfo {
 public:
  static void begin();

  static const char* uuid();        // "sh-XXXXXXXXXXXX" (lowercase MAC)
  static const char* macAddress();  // "AA:BB:CC:DD:EE:FF"
  static const char* shortId();     // last 4 hex of MAC, for SSID / mDNS
  static const char* firmwareVersion();
  static const char* hardwareRevision();
  static const char* chipModel();

  static uint8_t  chipRevision();
  static uint8_t  chipCores();
  static uint32_t cpuFreqMhz();
  static uint32_t flashSizeBytes();
  static uint32_t sketchSizeBytes();
  static uint32_t freeSketchSpaceBytes();

  static uint32_t freeHeapBytes();
  static uint32_t minFreeHeapBytes();
  static uint32_t maxAllocHeapBytes();
  static uint32_t heapFragmentationPct();

  static uint64_t uptimeMs();
  static const char* resetReason();     // human readable, per core 0
  static bool     lastResetWasBrownout();  // the 5 V supply smoking gun

  /// Full registration payload: UUID, MAC, firmware, relay count, chip info,
  /// memory, flash, uptime and capabilities. Written as JSON into `out`.
  static size_t toRegistrationJson(char* out, size_t cap);

  /// Compact runtime snapshot used by the 30 s heartbeat.
  static size_t toDiagnosticsJson(char* out, size_t cap);
};

}  // namespace sh
