#include "security/Security.h"

#include <Arduino.h>
#include <esp_random.h>
#include <mbedtls/md.h>
#include <stdio.h>
#include <string.h>

#include "config/ConfigStore.h"
#include "log/Logger.h"
#include "storage/NvsStore.h"

namespace sh {
namespace {

constexpr const char* TAG = "sec";
constexpr const char* kKeyApi = "apikey";
constexpr const char* kKeyClaim = "claim";
constexpr const char* kKeyClaimed = "claimed";
constexpr const char* kKeyCounterOut = "ctrOut";
constexpr const char* kKeyCounterIn = "ctrIn";

/// The outbound counter is persisted in blocks. Writing it on every message
/// would burn NVS; instead we reserve a block, use it from RAM, and only touch
/// flash when the block is exhausted. A power loss therefore skips forward -
/// which is fine, because the requirement is "strictly increasing", not
/// "gapless".
constexpr uint32_t kCounterBlock = 100;

char g_apiKey[Security::kApiKeyLen + 1] = {0};
char g_claimCode[Security::kClaimCodeLen + 1] = {0};
bool g_claimed = false;

uint32_t g_counter = 0;        // next value to hand out
uint32_t g_counterReserved = 0;  // persisted high-water mark
uint32_t g_inboundHigh = 0;

/// Unambiguous alphabet: no O/0, I/1, S/5, B/8. People read these aloud.
constexpr char kClaimAlphabet[] = "ACDEFGHJKLMNPQRTUVWXYZ2346789";

void reserveCounterBlock() {
  g_counterReserved += kCounterBlock;
  NvsStore::putU32(nvsns::kSecurity, kKeyCounterOut, g_counterReserved);
}

void toHex(const uint8_t* in, size_t len, char* out) {
  static const char* kHex = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    out[i * 2] = kHex[(in[i] >> 4) & 0x0F];
    out[i * 2 + 1] = kHex[in[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

}  // namespace

void Security::begin() {
  NvsStore::getString(nvsns::kSecurity, kKeyApi, g_apiKey, sizeof(g_apiKey), "");
  NvsStore::getString(nvsns::kSecurity, kKeyClaim, g_claimCode, sizeof(g_claimCode), "");
  g_claimed = NvsStore::getBool(nvsns::kSecurity, kKeyClaimed, false);

  g_counterReserved = NvsStore::getU32(nvsns::kSecurity, kKeyCounterOut, 0);
  // Start at the reserved high-water mark: anything below it may already have
  // been used before the last power loss.
  g_counter = g_counterReserved;
  reserveCounterBlock();

  g_inboundHigh = NvsStore::getU32(nvsns::kSecurity, kKeyCounterIn, 0);

  if (g_claimCode[0] == '\0') regenerateClaimCode();

  SH_LOGI(TAG, "claimed=%s key=%s claimCode=%s counter=%lu",
          g_claimed ? "yes" : "no", g_apiKey[0] ? "present" : "none", g_claimCode,
          static_cast<unsigned long>(g_counter));
}

bool Security::hasApiKey() { return g_apiKey[0] != '\0'; }
const char* Security::apiKey() { return g_apiKey; }

bool Security::setApiKey(const char* key) {
  if (!key || strlen(key) < 16 || strlen(key) > kApiKeyLen) {
    SH_LOGE(TAG, "rejected api key of invalid length");
    return false;
  }
  strncpy(g_apiKey, key, sizeof(g_apiKey) - 1);
  g_apiKey[sizeof(g_apiKey) - 1] = '\0';
  return NvsStore::putString(nvsns::kSecurity, kKeyApi, g_apiKey);
}

void Security::clearApiKey() {
  memset(g_apiKey, 0, sizeof(g_apiKey));
  NvsStore::remove(nvsns::kSecurity, kKeyApi);
}

bool Security::isClaimed() { return g_claimed; }
const char* Security::claimCode() { return g_claimCode; }

void Security::regenerateClaimCode() {
  const size_t alphabet = sizeof(kClaimAlphabet) - 1;
  for (size_t i = 0; i < kClaimCodeLen; ++i) {
    g_claimCode[i] = kClaimAlphabet[esp_random() % alphabet];
  }
  g_claimCode[kClaimCodeLen] = '\0';
  NvsStore::putString(nvsns::kSecurity, kKeyClaim, g_claimCode);
  SH_LOGI(TAG, "claim code: %s", g_claimCode);
}

bool Security::markClaimed(const char* key, const char* homeId, const char* roomId) {
  if (!setApiKey(key)) return false;
  g_claimed = true;
  NvsStore::putBool(nvsns::kSecurity, kKeyClaimed, true);
  ConfigStore::setIds(homeId, roomId);
  SH_LOGI(TAG, "device claimed (home=%s room=%s)", homeId ? homeId : "-",
          roomId ? roomId : "-");
  return true;
}

void Security::unclaim() {
  g_claimed = false;
  clearApiKey();
  NvsStore::putBool(nvsns::kSecurity, kKeyClaimed, false);
  regenerateClaimCode();
  SH_LOGW(TAG, "device unclaimed");
}

bool Security::signHex(const char* data, size_t len, char* outHex, size_t cap) {
  if (!hasApiKey() || !data || !outHex || cap < 65) return false;

  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  bool ok = false;
  uint8_t digest[32];

  if (mbedtls_md_setup(&ctx, info, 1) == 0 &&
      mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const uint8_t*>(g_apiKey),
                             strlen(g_apiKey)) == 0 &&
      mbedtls_md_hmac_update(&ctx, reinterpret_cast<const uint8_t*>(data), len) == 0 &&
      mbedtls_md_hmac_finish(&ctx, digest) == 0) {
    toHex(digest, sizeof(digest), outHex);
    ok = true;
  }
  mbedtls_md_free(&ctx);
  return ok;
}

bool Security::signRequest(const char* verb, const char* resource, const char* body,
                           uint32_t counter, char* outHex, size_t cap) {
  if (!hasApiKey()) return false;

  const size_t need = strlen(verb ? verb : "") + strlen(resource ? resource : "") +
                      strlen(body ? body : "") + 24;
  char* canonical = static_cast<char*>(malloc(need));
  if (!canonical) return false;

  snprintf(canonical, need, "%s\n%s\n%lu\n%s", verb ? verb : "",
           resource ? resource : "", static_cast<unsigned long>(counter),
           body ? body : "");
  const bool ok = signHex(canonical, strlen(canonical), outHex, cap);
  free(canonical);
  return ok;
}

uint32_t Security::nextCounter() {
  const uint32_t v = ++g_counter;
  if (v >= g_counterReserved) reserveCounterBlock();
  return v;
}

uint32_t Security::currentCounter() { return g_counter; }

bool Security::acceptInboundCounter(uint32_t counter) {
  if (counter <= g_inboundHigh) {
    SH_LOGW(TAG, "replay rejected: counter %lu <= %lu",
            static_cast<unsigned long>(counter),
            static_cast<unsigned long>(g_inboundHigh));
    return false;
  }
  g_inboundHigh = counter;
  // Same block trick as outbound: only persist when we cross a boundary.
  if ((counter % kCounterBlock) == 0) {
    NvsStore::putU32(nvsns::kSecurity, kKeyCounterIn, counter);
  }
  return true;
}

bool Security::secureEquals(const char* a, const char* b) {
  if (!a || !b) return false;
  const size_t la = strlen(a);
  const size_t lb = strlen(b);
  // Compare over a fixed span so the loop count does not leak the length.
  const size_t n = la > lb ? la : lb;
  uint8_t diff = static_cast<uint8_t>(la ^ lb);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t ca = i < la ? static_cast<uint8_t>(a[i]) : 0;
    const uint8_t cb = i < lb ? static_cast<uint8_t>(b[i]) : 0;
    diff |= static_cast<uint8_t>(ca ^ cb);
  }
  return diff == 0;
}

void Security::randomHex(char* out, size_t chars) {
  static const char* kHex = "0123456789abcdef";
  for (size_t i = 0; i < chars; ++i) out[i] = kHex[esp_random() & 0x0F];
  out[chars] = '\0';
}

}  // namespace sh
