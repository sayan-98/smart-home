// ---------------------------------------------------------------------------
//  AlexaLocal.h - Philips Hue bridge emulation, so an Echo controls the relays
//  directly over the LAN.
//
//  Why this and not the cloud Alexa Smart Home Skill (which is Phase 4):
//   - It costs nothing: no Amazon developer account, no AWS Lambda, no OAuth
//     account linking, no public HTTPS endpoint.
//   - It works with the internet DOWN. Echo -> ESP32 is a local call.
//   - It is faster: no round trip through Amazon's cloud.
//  Its one limitation is that it only works while you are at home, on the same
//  network. Voice control from outside the house needs the cloud skill.
//
//  How discovery works:
//   1. Echo broadcasts an SSDP M-SEARCH to 239.255.255.250:1900.
//   2. We answer with a Hue-bridge advertisement pointing at /description.xml.
//   3. Echo fetches the description, "presses the link button" (POST /api),
//      then reads /api/<user>/lights and creates one device per relay.
//
//  REQUIREMENTS ON THE NETWORK (these are the usual failure causes):
//   - Echo and ESP32 on the SAME subnet. The ESP32 is 2.4 GHz only; if your
//     router splits the bands into separate SSIDs, put the Echo on 2.4 GHz.
//   - AP/client isolation OFF on the router.
//   - Multicast/SSDP not filtered - some mesh systems drop it by default.
//
//  The Hue API shares port 80 with LocalApi. Hue paths look like
//  /api/<username>/lights/... and never collide with this firmware's own
//  /api/<known-name> routes, so they are picked up from the not-found path.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

class AsyncWebServer;
class AsyncWebServerRequest;

namespace sh {

class AlexaLocal {
 public:
  static void begin();

  /// Registers the routes this module owns on the shared web server.
  static void attach(AsyncWebServer& server);

  /// Called from LocalApi's not-found handler. Returns true when the request
  /// was a Hue API call and has been answered.
  static bool handleUnmatched(AsyncWebServerRequest* req, const char* body);

  /// (Re)starts the SSDP responder. Safe to call on every Wi-Fi reconnect.
  static void onNetworkUp();
  static void onNetworkDown();

  static bool isEnabled();
  static uint32_t discoveryRequests();  // diagnostics: has the Echo ever asked?

  /// "AABBCCFFFEDDEEFF" - what the Hue protocol calls the bridge id.
  static const char* bridgeId();
};

}  // namespace sh
