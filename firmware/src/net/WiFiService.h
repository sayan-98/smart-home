// ---------------------------------------------------------------------------
//  WiFiService.h - multi-network station manager with a provisioning fallback.
//
//  Behaviour:
//   - Up to kMaxNetworks saved credentials. On connect it scans and picks the
//     strongest network it knows, rather than blindly trying the first.
//   - Exponential backoff with jitter between attempts (1 s -> 60 s).
//   - After kFailuresBeforeAp consecutive failures it raises the SoftAP
//     *alongside* the station attempts, so a device that moved house is
//     recoverable without a serial cable - and still reconnects on its own if
//     the original network comes back.
//   - The SoftAP is WPA2 with a per-device password derived from the MAC
//     (Loophole #17); an open portal would hand your Wi-Fi password to anyone
//     in range during setup.
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <IPAddress.h>

namespace sh {

class WiFiService {
 public:
  static constexpr uint8_t kMaxNetworks = 5;
  static constexpr uint8_t kFailuresBeforeAp = 3;

  static void begin();
  static void startTask();

  static bool isConnected();
  static bool isApActive();
  static int32_t rssi();
  static IPAddress localIp();
  static IPAddress apIp();
  static const char* ssid();
  static const char* hostname();

  /// SoftAP identity. The password is deterministic per device so it can be
  /// printed on the enclosure and survives a factory reset.
  static const char* apSsid();
  static const char* apPassword();

  /// Saves a network and triggers an immediate reconnect. Existing entries
  /// with the same SSID are replaced; the list is LRU-evicted when full.
  static bool addNetwork(const char* ssid, const char* password);
  static bool forgetNetwork(const char* ssid);
  static void forgetAll();
  static uint8_t savedNetworkCount();
  static size_t savedNetworksToJson(char* out, size_t cap);

  /// Scan results for the portal's network picker.
  ///
  /// Scanning takes seconds and MUST NOT happen on the AsyncTCP task - that
  /// would stall every HTTP connection and trip the async watchdog. So the
  /// scan runs on the Wi-Fi task and this returns the cached result plus a
  /// `scanning` flag; the portal polls until it clears.
  static size_t scanToJson(char* out, size_t cap);
  static void requestScan();
  static bool scanInProgress();

  static void requestReconnect();
  static void startAp();
  static void stopAp();

 private:
  static void taskEntry(void* arg);
};

}  // namespace sh
