#include "ota/OtaService.h"

#include <ArduinoJson.h>
#include <Arduino.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "config/ConfigStore.h"
#include "core/EventBus.h"
#include "core/StatusLed.h"
#include "device/DeviceInfo.h"
#include "log/Logger.h"
#include "relay/RelayManager.h"
#include "security/Security.h"

namespace sh {
namespace {

constexpr const char* TAG = "ota";
constexpr size_t kChunk = 2048;
constexpr int kHttpTimeoutMs = 20000;
constexpr int kMaxRedirects = 5;

volatile OtaState g_state = OtaState::Idle;
volatile uint8_t  g_progress = 0;
char g_error[96] = {0};
TaskHandle_t g_task = nullptr;
bool g_pendingVerify = false;

struct Job {
  char url[224];
  char sha256[65];
  bool fromManifest;
};

Job g_job{};

void fail(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_error, sizeof(g_error), fmt, ap);
  va_end(ap);
  g_state = OtaState::Failed;
  SH_LOGE(TAG, "%s", g_error);

  Event e;
  e.type = EventType::OtaFailed;
  EventBus::publish(e);
}

void hexToBytes(const char* hex, uint8_t* out, size_t outLen) {
  for (size_t i = 0; i < outLen; ++i) {
    unsigned v = 0;
    sscanf(hex + i * 2, "%2x", &v);
    out[i] = static_cast<uint8_t>(v);
  }
}

/// Semantic-ish version compare. Returns >0 when `remote` is newer.
int versionCompare(const char* remote, const char* local) {
  int r[3] = {0, 0, 0}, l[3] = {0, 0, 0};
  sscanf(remote, "%d.%d.%d", &r[0], &r[1], &r[2]);
  sscanf(local, "%d.%d.%d", &l[0], &l[1], &l[2]);
  for (int i = 0; i < 3; ++i) {
    if (r[i] != l[i]) return r[i] - l[i];
  }
  return 0;
}

/// Opens `url`, following redirects (Supabase Storage and Render both issue
/// them), and leaves the handle positioned at the body. Returns content length,
/// or -1 on failure. Caller must cleanup the handle either way.
int openWithRedirects(esp_http_client_handle_t& client, const char* url,
                      bool sendAuth) {
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.timeout_ms = kHttpTimeoutMs;
  cfg.method = HTTP_METHOD_GET;
  cfg.disable_auto_redirect = true;  // handled explicitly so we can re-open
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 1024;
  // The bundled Mozilla root store rather than one pinned CA: a pinned root
  // that expires would cut the fleet off from the very channel needed to fix
  // it (Loophole #18). The bundle travels with the firmware.
  cfg.crt_bundle_attach = esp_crt_bundle_attach;

  client = esp_http_client_init(&cfg);
  if (!client) {
    fail("http client init failed");
    return -1;
  }
  if (sendAuth && Security::hasApiKey()) {
    esp_http_client_set_header(client, "X-API-Key", Security::apiKey());
    esp_http_client_set_header(client, "X-Device-Uuid", DeviceInfo::uuid());
  }

  for (int hop = 0; hop <= kMaxRedirects; ++hop) {
    const esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      fail("connect failed: %s", esp_err_to_name(err));
      return -1;
    }
    const int len = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);

    if (status == 301 || status == 302 || status == 303 || status == 307 ||
        status == 308) {
      if (hop == kMaxRedirects) {
        fail("too many redirects");
        return -1;
      }
      esp_http_client_set_redirection(client);
      esp_http_client_close(client);
      continue;
    }
    if (status != 200) {
      fail("HTTP %d", status);
      return -1;
    }
    return len;  // may be -1 for chunked responses
  }
  fail("redirect loop");
  return -1;
}

/// Streams the image into the inactive slot, hashing as it goes.
bool downloadAndFlash(const char* url, const char* expectedSha) {
  const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
  if (!target) {
    fail("no OTA partition available");
    return false;
  }
  SH_LOGI(TAG, "target partition '%s' (%u KB)", target->label,
          static_cast<unsigned>(target->size / 1024));

  esp_http_client_handle_t client = nullptr;
  const int total = openWithRedirects(client, url, /*sendAuth=*/false);
  if (total < 0) {
    if (client) esp_http_client_cleanup(client);
    return false;
  }
  if (total == 0 || static_cast<size_t>(total) > target->size) {
    fail("image size %d does not fit the %u KB partition", total,
         static_cast<unsigned>(target->size / 1024));
    esp_http_client_cleanup(client);
    return false;
  }

  esp_ota_handle_t handle = 0;
  esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
  if (err != ESP_OK) {
    fail("esp_ota_begin: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  uint8_t* buf = static_cast<uint8_t*>(malloc(kChunk));
  if (!buf) {
    fail("out of memory for the download buffer");
    esp_ota_abort(handle);
    mbedtls_sha256_free(&sha);
    esp_http_client_cleanup(client);
    return false;
  }

  auto abortAll = [&](const char* why, int at) {
    fail("%s at %d/%d bytes", why, at, total);
    free(buf);
    esp_ota_abort(handle);
    mbedtls_sha256_free(&sha);
    esp_http_client_cleanup(client);
  };

  g_state = OtaState::Downloading;
  int written = 0;

  while (written < total) {
    const int got = esp_http_client_read(client, reinterpret_cast<char*>(buf), kChunk);
    if (got < 0) {
      abortAll("read error", written);
      return false;
    }
    if (got == 0) {
      if (esp_http_client_is_complete_data_received(client)) break;
      abortAll("connection closed early", written);
      return false;
    }

    if (esp_ota_write(handle, buf, static_cast<size_t>(got)) != ESP_OK) {
      abortAll("flash write failed", written);
      return false;
    }
    mbedtls_sha256_update(&sha, buf, static_cast<size_t>(got));
    written += got;

    const uint8_t pct = static_cast<uint8_t>((static_cast<int64_t>(written) * 100) / total);
    if (pct != g_progress) {
      g_progress = pct;
      if (pct % 10 == 0) SH_LOGI(TAG, "downloaded %u%%", static_cast<unsigned>(pct));
      Event e;
      e.type = EventType::OtaProgress;
      e.value = pct;
      EventBus::publish(e);
    }
  }

  free(buf);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (written != total) {
    fail("short download: %d of %d bytes", written, total);
    esp_ota_abort(handle);
    mbedtls_sha256_free(&sha);
    return false;
  }

  // --- integrity check BEFORE touching any boot flag ----------------------
  g_state = OtaState::Verifying;
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);

  if (expectedSha && strlen(expectedSha) == 64) {
    uint8_t expect[32];
    hexToBytes(expectedSha, expect, sizeof(expect));
    if (memcmp(digest, expect, sizeof(digest)) != 0) {
      fail("SHA-256 mismatch - image rejected, nothing was activated");
      esp_ota_abort(handle);
      return false;
    }
    SH_LOGI(TAG, "SHA-256 verified");
  } else {
    SH_LOGW(TAG, "manifest carried no checksum - integrity unverified");
  }

  err = esp_ota_end(handle);
  if (err != ESP_OK) {
    // esp_ota_end also validates the image header; a corrupt build fails here.
    fail("esp_ota_end: %s", esp_err_to_name(err));
    return false;
  }

  g_state = OtaState::Applying;
  err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    fail("set_boot_partition: %s", esp_err_to_name(err));
    return false;
  }

  SH_LOGI(TAG, "update staged on '%s'", target->label);
  g_state = OtaState::PendingReboot;

  Event e;
  e.type = EventType::OtaSucceeded;
  EventBus::publish(e);
  return true;
}

bool fetchManifest(Job& job, bool force) {
  const DeviceConfig& cfg = ConfigStore::device();
  if (!cfg.apiBase[0]) {
    fail("no update server configured");
    return false;
  }

  char url[320];
  snprintf(url, sizeof(url), "%s/api/ota/manifest?uuid=%s&hw=%s&fw=%s", cfg.apiBase,
           DeviceInfo::uuid(), DeviceInfo::hardwareRevision(),
           DeviceInfo::firmwareVersion());

  esp_http_client_handle_t client = nullptr;
  const int len = openWithRedirects(client, url, /*sendAuth=*/true);
  if (len < 0 && len != -1) {
    if (client) esp_http_client_cleanup(client);
    return false;
  }
  if (!client) return false;

  char body[768];
  const int got = esp_http_client_read_response(client, body, sizeof(body) - 1);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (got <= 0) {
    fail("empty manifest");
    return false;
  }
  body[got] = '\0';

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    fail("manifest is not valid json");
    return false;
  }

  const char* version = doc["version"] | "";
  const char* imageUrl = doc["url"] | "";
  const char* sha = doc["sha256"] | "";
  if (!version[0] || !imageUrl[0]) {
    fail("manifest missing version or url");
    return false;
  }

  if (!force && versionCompare(version, DeviceInfo::firmwareVersion()) <= 0) {
    SH_LOGI(TAG, "already up to date (%s)", DeviceInfo::firmwareVersion());
    g_state = OtaState::Idle;
    return false;
  }

  SH_LOGI(TAG, "update available: %s -> %s", DeviceInfo::firmwareVersion(), version);
  strncpy(job.url, imageUrl, sizeof(job.url) - 1);
  strncpy(job.sha256, sha, sizeof(job.sha256) - 1);
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------

void OtaService::begin() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (running && esp_ota_get_state_partition(running, &st) == ESP_OK) {
    g_pendingVerify = (st == ESP_OTA_IMG_PENDING_VERIFY);
    if (g_pendingVerify) {
      SH_LOGW(TAG,
              "running an UNCONFIRMED image on '%s' - it rolls back on the next "
              "reset unless it proves it can get online",
              running->label);
    } else {
      SH_LOGI(TAG, "running confirmed image on '%s'", running->label);
    }
  }
}

void OtaService::markCurrentImageValid() {
  if (!g_pendingVerify) return;
  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    g_pendingVerify = false;
    SH_LOGI(TAG, "image confirmed - rollback cancelled");
  } else {
    SH_LOGE(TAG, "could not confirm image: %s", esp_err_to_name(err));
  }
}

bool OtaService::isPendingVerify() { return g_pendingVerify; }
OtaState OtaService::state() { return g_state; }
uint8_t OtaService::progressPercent() { return g_progress; }
const char* OtaService::lastError() { return g_error; }

const char* OtaService::runningPartition() {
  const esp_partition_t* p = esp_ota_get_running_partition();
  return p ? p->label : "?";
}

bool OtaService::checkAndUpdate(bool force) {
  if (g_task) {
    SH_LOGW(TAG, "an update is already in progress");
    return false;
  }
  memset(&g_job, 0, sizeof(g_job));
  g_job.fromManifest = true;
  g_progress = 0;
  g_error[0] = '\0';
  g_state = OtaState::Checking;

  // 8 KB: the TLS handshake, HTTP client and flash driver all live here.
  return xTaskCreatePinnedToCore(
             &OtaService::taskEntry, "ota", 8192,
             reinterpret_cast<void*>(static_cast<uintptr_t>(force ? 1 : 0)), 1,
             &g_task, 0) == pdPASS;
}

bool OtaService::updateFrom(const char* url, const char* sha256Hex) {
  if (g_task || !url) return false;
  memset(&g_job, 0, sizeof(g_job));
  g_job.fromManifest = false;
  strncpy(g_job.url, url, sizeof(g_job.url) - 1);
  if (sha256Hex) strncpy(g_job.sha256, sha256Hex, sizeof(g_job.sha256) - 1);
  g_progress = 0;
  g_error[0] = '\0';
  g_state = OtaState::Checking;

  return xTaskCreatePinnedToCore(&OtaService::taskEntry, "ota", 8192, nullptr, 1,
                                 &g_task, 0) == pdPASS;
}

void OtaService::taskEntry(void* arg) {
  // Deliberately NOT watchdog-subscribed: a flash erase can exceed the
  // watchdog period, and a reset mid-write is how a device gets bricked.
  const bool force = (reinterpret_cast<uintptr_t>(arg) != 0);

  StatusLed::set(LedPattern::Ota);
  {
    Event e;
    e.type = EventType::OtaStarted;
    EventBus::publish(e);
  }

  bool ok = false;
  if (!g_job.fromManifest || fetchManifest(g_job, force)) {
    // Persist relay state before the swap, so the new image restores exactly
    // what was on.
    RelayManager::flush();
    ok = downloadAndFlash(g_job.url, g_job.sha256[0] ? g_job.sha256 : nullptr);
  }

  g_task = nullptr;

  if (ok) {
    SH_LOGW(TAG, "rebooting into the new image");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
  }

  StatusLed::set(LedPattern::OnlineLocal);
  vTaskDelete(nullptr);
}

}  // namespace sh
