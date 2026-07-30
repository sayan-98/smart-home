// ---------------------------------------------------------------------------
//  Security.h - device credentials, payload signing and replay protection.
//
//  Design notes (and their honest limits):
//
//   * The device holds a long-lived API KEY, not a JWT. A JWT expires, and a
//     device that has been offline for a month cannot refresh one - it would
//     lock itself out permanently (Loophole #12). The key is revocable
//     server-side, which is the property that actually matters.
//
//   * Every cloud payload is signed HMAC-SHA256 over
//         <method>\n<path-or-topic>\n<counter>\n<body>
//     and carries a monotonic counter that is PERSISTED. A counter that reset
//     on every power cycle would make replay protection decorative
//     (Loophole #14).
//
//   * The claim code is what stops anyone from adopting your device just by
//     knowing its MAC (Loophole #11). The device self-registers but stays
//     inert until a logged-in user types the code shown in the portal.
//
//   * NVS here is NOT encrypted. Anyone with physical access and a USB cable
//     can read this key out of flash. Real protection needs eFuse flash
//     encryption + secure boot, which are irreversible per chip and are a
//     deliberate Phase 4 decision (Loophole #13). Treat the key as
//     "revocable", not "secret from someone holding the board".
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sh {

class Security {
 public:
  static constexpr size_t kApiKeyLen = 64;    // hex chars
  static constexpr size_t kClaimCodeLen = 8;

  static void begin();

  // --- device API key ------------------------------------------------------
  static bool hasApiKey();
  static const char* apiKey();
  static bool setApiKey(const char* key);
  static void clearApiKey();

  // --- claim flow ----------------------------------------------------------
  static bool isClaimed();
  static const char* claimCode();      // regenerated on factory reset
  static void regenerateClaimCode();
  static bool markClaimed(const char* apiKey, const char* homeId, const char* roomId);
  static void unclaim();

  // --- signing -------------------------------------------------------------

  /// Hex-encoded HMAC-SHA256 of `data` under the device API key.
  /// `outHex` must hold at least 65 bytes. False if no key is provisioned.
  static bool signHex(const char* data, size_t len, char* outHex, size_t cap);

  /// Convenience: builds the canonical string and signs it.
  /// canonical = "<verb>\n<resource>\n<counter>\n<body>"
  static bool signRequest(const char* verb, const char* resource, const char* body,
                          uint32_t counter, char* outHex, size_t cap);

  /// Next outbound counter. Persisted, survives power loss, never repeats.
  static uint32_t nextCounter();
  static uint32_t currentCounter();

  /// Validates an inbound counter from the backend (must strictly increase).
  /// Rejects replays of previously seen messages.
  static bool acceptInboundCounter(uint32_t counter);

  /// Constant-time comparison, for API key / signature checks.
  static bool secureEquals(const char* a, const char* b);

  /// Random hex string, from the hardware RNG.
  static void randomHex(char* out, size_t chars);
};

}  // namespace sh
