// ---------------------------------------------------------------------------
//  LocalApi.h - the on-device HTTP + WebSocket server.
//
//  This is what makes "offline-first" real rather than a slogan. With the
//  internet down, the router still routes: the app (or any browser) reaches
//  http://smarthome-XXXX.local and has complete control - toggle, rename,
//  schedule, diagnose. Nothing here depends on the cloud, the broker, or DNS
//  beyond mDNS.
//
//  It also hosts the captive portal while the provisioning AP is up.
//
//  AUTH MODEL (pragmatic v1, and its limits):
//    * Unclaimed device -> the API is open on the LAN. It has to be: this is
//      how you set it up in the first place, and it holds nothing worth
//      stealing yet.
//    * Claimed device -> mutating requests need either the device API key in
//      `X-API-Key`, or a session cookie obtained by POSTing the claim code
//      (printed on the enclosure) to /api/local-auth. Reads stay open on the
//      LAN so the wall-mounted dashboard keeps working.
//    * This is plain HTTP on your LAN. Anyone already inside your network can
//      read the traffic. TLS on the LAN path is Phase 4 (Loophole #8).
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

namespace sh {

class LocalApi {
 public:
  static void begin();
  static void startTask();

  static bool isRunning();
  static uint32_t connectedClients();

  /// Pushes a full state snapshot to every connected WebSocket client.
  static void broadcastSnapshot();

 private:
  static void taskEntry(void* arg);
};

}  // namespace sh
