// ---------------------------------------------------------------------------
//  OtaService.h - HTTPS pull updates with real rollback.
//
//  The partition table gives two 1.6 MB app slots. An update writes the
//  inactive slot, marks it for boot, and reboots. The crucial part is what
//  happens next: the new image is only marked VALID after it has proved it
//  works - specifically after it boots, associates to Wi-Fi and (when the
//  cloud is enabled) reaches the broker. If it panics or never gets that far,
//  the bootloader falls back to the previous slot on the next reset.
//
//  Marking valid merely because setup() ran would defeat the entire point: the
//  classic bad OTA is one that boots fine and then cannot reach the network,
//  which is exactly the case that must roll back.
//
//  Integrity: the manifest carries a SHA-256 of the image, verified while
//  streaming. A truncated or corrupted download never reaches the boot flags.
//
//  The task is created on demand and destroyed afterwards - an 8 KB stack that
//  exists only during an update. It is deliberately NOT watchdog-subscribed:
//  a flash write can outlast the watchdog period, and a reboot mid-write is
//  how devices get bricked.
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sh {

enum class OtaState : uint8_t {
  Idle = 0, Checking, Downloading, Verifying, Applying, PendingReboot, Failed
};

class OtaService {
 public:
  static void begin();

  /// Confirms the running image after it has proven itself. Called once the
  /// device is online (and connected to the broker, if the cloud is enabled).
  /// Until this runs, a reset rolls back to the previous firmware.
  static void markCurrentImageValid();

  /// True when running an image that has not yet been confirmed.
  static bool isPendingVerify();

  /// Asks the update server whether a newer build exists, and installs it.
  /// Non-blocking: spawns the OTA task and returns.
  static bool checkAndUpdate(bool force = false);

  /// Installs from an explicit URL (used by the "ota" cloud command).
  static bool updateFrom(const char* url, const char* sha256Hex);

  static OtaState state();
  static uint8_t  progressPercent();
  static const char* lastError();
  static const char* runningPartition();

 private:
  static void taskEntry(void* arg);
};

}  // namespace sh
