#include "net/WiFiService.h"

#include <ArduinoJson.h>
#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <ctype.h>
#include <esp_random.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "config/ConfigStore.h"
#include "core/EventBus.h"
#include "device/DeviceInfo.h"
#include "log/Logger.h"
#include "net/TimeService.h"
#include "storage/NvsStore.h"
#include "util/Backoff.h"
#include "util/Crc32.h"

namespace sh {
namespace {

constexpr const char* TAG = "wifi";
constexpr const char* kCredKey = "nets";
constexpr uint32_t kMagic = 0x53485746u;  // 'SHWF'
constexpr uint16_t kVersion = 1;
constexpr uint32_t kConnectTimeoutMs = 15000;

struct __attribute__((packed)) Cred {
  char ssid[33];
  char pass[65];
};

struct __attribute__((packed)) CredStore {
  uint32_t magic;
  uint16_t version;
  uint8_t  count;
  Cred     nets[WiFiService::kMaxNetworks];
  uint32_t crc;
};

CredStore g_creds{};
TaskHandle_t g_task = nullptr;
Backoff g_backoff;

char g_hostname[32] = {0};
char g_apSsid[24] = {0};
char g_apPass[16] = {0};

volatile bool g_apActive = false;
volatile bool g_reconnectRequested = false;
uint8_t g_consecutiveFailures = 0;
bool g_mdnsUp = false;
bool g_wasConnected = false;

// --- cached scan results (owned by the Wi-Fi task) -------------------------
constexpr uint8_t kMaxScanEntries = 24;

struct ScanEntry {
  char    ssid[33];
  int32_t rssi;
  uint8_t channel;
  bool    secure;
  bool    known;
};

ScanEntry g_scan[kMaxScanEntries];
volatile uint8_t g_scanCount = 0;
volatile bool g_scanRequested = false;
volatile bool g_scanRunning = false;
uint32_t g_scanAtMs = 0;

/// A blocking scan takes seconds. Unsubscribe from the task watchdog for its
/// duration, otherwise the Wi-Fi task looks hung and the device reboots
/// mid-scan. RAII so no early return can leave the task unsubscribed.
struct WdtPause {
  WdtPause() { esp_task_wdt_delete(nullptr); }
  ~WdtPause() { esp_task_wdt_add(nullptr); }
};

void saveCreds() {
  g_creds.magic = kMagic;
  g_creds.version = kVersion;
  g_creds.crc = crc32(&g_creds, sizeof(CredStore) - sizeof(uint32_t));
  if (!NvsStore::putBlob(nvsns::kWifi, kCredKey, &g_creds, sizeof(g_creds))) {
    SH_LOGE(TAG, "failed to save networks");
  }
}

void loadCreds() {
  CredStore tmp{};
  const size_t n = NvsStore::getBlob(nvsns::kWifi, kCredKey, &tmp, sizeof(tmp));
  memset(&g_creds, 0, sizeof(g_creds));
  if (n != sizeof(tmp) || tmp.magic != kMagic || tmp.version != kVersion) return;
  if (crc32(&tmp, sizeof(CredStore) - sizeof(uint32_t)) != tmp.crc) {
    SH_LOGW(TAG, "saved networks failed CRC, discarding");
    return;
  }
  if (tmp.count > WiFiService::kMaxNetworks) return;
  g_creds = tmp;
  SH_LOGI(TAG, "%u saved network(s)", static_cast<unsigned>(g_creds.count));
}

int findCred(const char* ssid) {
  for (uint8_t i = 0; i < g_creds.count; ++i) {
    if (strncmp(g_creds.nets[i].ssid, ssid, sizeof(g_creds.nets[i].ssid)) == 0) {
      return i;
    }
  }
  return -1;
}

void deriveApIdentity() {
  snprintf(g_apSsid, sizeof(g_apSsid), "SmartHome-%s", DeviceInfo::shortId());
  // 8-char WPA2 key from the MAC. Deterministic on purpose: it can be printed
  // on the enclosure and it survives a factory reset.
  const char* mac = DeviceInfo::macAddress();  // "AA:BB:CC:DD:EE:FF"
  snprintf(g_apPass, sizeof(g_apPass), "SH%c%c%c%c%c%c", mac[9], mac[10], mac[12],
           mac[13], mac[15], mac[16]);
  snprintf(g_hostname, sizeof(g_hostname), "smarthome-%s", DeviceInfo::shortId());
  for (char* p = g_hostname; *p; ++p) *p = static_cast<char>(tolower(*p));
}

void startMdns() {
  if (g_mdnsUp) return;
  if (!MDNS.begin(g_hostname)) {
    SH_LOGW(TAG, "mDNS start failed");
    return;
  }
  MDNS.addService("http", "tcp", 80);
  MDNS.addServiceTxt("http", "tcp", "uuid", DeviceInfo::uuid());
  MDNS.addServiceTxt("http", "tcp", "fw", DeviceInfo::firmwareVersion());
  MDNS.addServiceTxt("http", "tcp", "ch", String(board::kChannelCount).c_str());
  g_mdnsUp = true;
  SH_LOGI(TAG, "mDNS up: http://%s.local", g_hostname);
}

/// Runs a blocking scan and refreshes the cache. Wi-Fi task only.
int performScan() {
  WdtPause pause;
  g_scanRunning = true;

  const int found = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  uint8_t n = 0;
  if (found > 0) {
    for (int i = 0; i < found && n < kMaxScanEntries; ++i) {
      const String s = WiFi.SSID(i);
      if (s.length() == 0) continue;  // hidden networks are not selectable here
      ScanEntry& e = g_scan[n];
      memset(&e, 0, sizeof(e));
      strncpy(e.ssid, s.c_str(), sizeof(e.ssid) - 1);
      e.rssi = WiFi.RSSI(i);
      e.channel = static_cast<uint8_t>(WiFi.channel(i));
      e.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      e.known = findCred(e.ssid) >= 0;
      n++;
    }
  }
  WiFi.scanDelete();

  g_scanCount = n;
  g_scanAtMs = millis();
  g_scanRunning = false;
  g_scanRequested = false;
  SH_LOGD(TAG, "scan complete, %u networks", static_cast<unsigned>(n));
  return found;
}

/// Refreshes the cache, then returns the index of the strongest saved network.
int pickBestKnown() {
  performScan();

  int bestCred = -1;
  int32_t bestRssi = -127;
  for (uint8_t i = 0; i < g_scanCount; ++i) {
    const int idx = findCred(g_scan[i].ssid);
    if (idx < 0) continue;
    if (g_scan[i].rssi > bestRssi) {
      bestRssi = g_scan[i].rssi;
      bestCred = idx;
    }
  }

  if (bestCred >= 0) {
    SH_LOGI(TAG, "best known network '%s' (%ld dBm)", g_creds.nets[bestCred].ssid,
            static_cast<long>(bestRssi));
  }
  return bestCred;
}

bool tryConnect(uint8_t credIndex) {
  const Cred& c = g_creds.nets[credIndex];
  SH_LOGI(TAG, "connecting to '%s'", c.ssid);

  WiFi.setHostname(g_hostname);
  WiFi.begin(c.ssid, c.pass);

  const uint32_t start = millis();
  while (millis() - start < kConnectTimeoutMs) {
    if (WiFi.status() == WL_CONNECTED) {
      SH_LOGI(TAG, "connected: ip=%s rssi=%ld", WiFi.localIP().toString().c_str(),
              static_cast<long>(WiFi.RSSI()));
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_task_wdt_reset();
  }
  SH_LOGW(TAG, "connect to '%s' timed out (status %d)", c.ssid,
          static_cast<int>(WiFi.status()));
  WiFi.disconnect(false, false);
  return false;
}

void onConnected() {
  g_consecutiveFailures = 0;
  g_backoff.reset();
  startMdns();
  TimeService::onNetworkUp();

  Event e;
  e.type = EventType::WifiConnected;
  e.value = WiFi.RSSI();
  EventBus::publish(e);

  // Once the station is up the AP is only clutter - and leaving it running
  // costs heap and keeps a second DHCP server alive on the same box.
  if (g_apActive && g_creds.count > 0) {
    WiFiService::stopAp();
  }
}

void onDisconnected() {
  TimeService::onNetworkDown();
  Event e;
  e.type = EventType::WifiDisconnected;
  EventBus::publish(e);
}

}  // namespace

void WiFiService::begin() {
  deriveApIdentity();
  loadCreds();

  WiFi.persistent(false);   // we manage credentials ourselves, in NVS
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);     // sleep adds tens of ms to every command round trip

  g_backoff.configure(1000, 60000, 25);

  SH_LOGI(TAG, "host=%s ap=%s pass=%s", g_hostname, g_apSsid, g_apPass);
}

void WiFiService::startTask() {
  if (g_task) return;
  xTaskCreatePinnedToCore(&WiFiService::taskEntry, "wifi", 4096, nullptr, 3,
                          &g_task, 0);
}

void WiFiService::taskEntry(void* /*arg*/) {
  esp_task_wdt_add(nullptr);

  // No credentials at all: this is a fresh device, go straight to provisioning.
  if (g_creds.count == 0) {
    SH_LOGW(TAG, "no saved networks, starting provisioning AP");
    startAp();
  }

  for (;;) {
    esp_task_wdt_reset();

    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != g_wasConnected) {
      g_wasConnected = connected;
      connected ? onConnected() : onDisconnected();
    }

    // Portal scan requests are serviced here, on this task - never on the
    // AsyncTCP task that the web server runs on.
    if (g_scanRequested && !g_scanRunning) {
      performScan();
      continue;
    }

    if (connected && !g_reconnectRequested) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (g_reconnectRequested) {
      g_reconnectRequested = false;
      g_backoff.reset();
      g_consecutiveFailures = 0;
      WiFi.disconnect(false, false);
      vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (g_creds.count == 0) {
      // Provisioning: nothing to connect to yet, but the portal still needs
      // scan results, which the loop head services.
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    const int best = pickBestKnown();
    bool ok = false;
    if (best >= 0) {
      ok = tryConnect(static_cast<uint8_t>(best));
    } else {
      // None of our networks are in range right now. Still try the most
      // recently saved one - hidden SSIDs never show up in a scan.
      SH_LOGD(TAG, "no known SSID in scan, trying most recent");
      ok = tryConnect(0);
    }

    if (ok) continue;  // loop head will publish WifiConnected

    if (g_consecutiveFailures < 0xFF) g_consecutiveFailures++;
    if (g_consecutiveFailures >= kFailuresBeforeAp && !g_apActive) {
      SH_LOGW(TAG, "%u consecutive failures, raising recovery AP",
              static_cast<unsigned>(g_consecutiveFailures));
      startAp();
    }

    const uint32_t wait = g_backoff.next(esp_random());
    SH_LOGI(TAG, "retry in %lu ms", static_cast<unsigned long>(wait));

    // Sleep in slices so the watchdog stays fed and a reconnect request is
    // picked up promptly instead of after a full 60 s backoff.
    uint32_t slept = 0;
    while (slept < wait && !g_reconnectRequested) {
      vTaskDelay(pdMS_TO_TICKS(250));
      slept += 250;
      esp_task_wdt_reset();
    }
  }
}

bool WiFiService::isConnected() { return WiFi.status() == WL_CONNECTED; }
bool WiFiService::isApActive() { return g_apActive; }
int32_t WiFiService::rssi() { return isConnected() ? WiFi.RSSI() : 0; }
IPAddress WiFiService::localIp() { return WiFi.localIP(); }
IPAddress WiFiService::apIp() { return WiFi.softAPIP(); }
const char* WiFiService::hostname() { return g_hostname; }
const char* WiFiService::apSsid() { return g_apSsid; }
const char* WiFiService::apPassword() { return g_apPass; }

const char* WiFiService::ssid() {
  static String s;
  s = WiFi.SSID();
  return s.c_str();
}

bool WiFiService::addNetwork(const char* ssid, const char* password) {
  if (!ssid || !ssid[0]) return false;
  const size_t slen = strlen(ssid);
  if (slen > 32) return false;
  const size_t plen = password ? strlen(password) : 0;
  if (plen > 64) return false;

  int idx = findCred(ssid);
  if (idx < 0) {
    if (g_creds.count < kMaxNetworks) {
      idx = g_creds.count++;
    } else {
      // Full: evict the oldest (index 0) and shift down.
      memmove(&g_creds.nets[0], &g_creds.nets[1], sizeof(Cred) * (kMaxNetworks - 1));
      idx = kMaxNetworks - 1;
      SH_LOGW(TAG, "network list full, evicted oldest");
    }
  }

  memset(&g_creds.nets[idx], 0, sizeof(Cred));
  strncpy(g_creds.nets[idx].ssid, ssid, sizeof(g_creds.nets[idx].ssid) - 1);
  if (password) {
    strncpy(g_creds.nets[idx].pass, password, sizeof(g_creds.nets[idx].pass) - 1);
  }
  saveCreds();
  SH_LOGI(TAG, "saved network '%s' (%u total)", ssid,
          static_cast<unsigned>(g_creds.count));

  Event e;
  e.type = EventType::WifiProvisioned;
  EventBus::publish(e);

  // Only force a reconnect if we are not already on a network.
  //
  // Adding a second network usually happens over the first one - you are
  // standing on the page served by the device, on the Wi-Fi you are about to
  // supplement. Reconnecting unconditionally drops that connection and leaves
  // the page you are looking at dead, which reads as "adding a network broke
  // it". The new entry is picked up on the next natural reconnect anyway.
  if (!isConnected()) {
    requestReconnect();
  } else {
    SH_LOGI(TAG, "already connected to '%s', new network will be used on the next reconnect",
            WiFi.SSID().c_str());
  }
  return true;
}

bool WiFiService::forgetNetwork(const char* ssid) {
  const int idx = findCred(ssid);
  if (idx < 0) return false;
  for (uint8_t i = static_cast<uint8_t>(idx); i + 1 < g_creds.count; ++i) {
    g_creds.nets[i] = g_creds.nets[i + 1];
  }
  g_creds.count--;
  memset(&g_creds.nets[g_creds.count], 0, sizeof(Cred));
  saveCreds();
  return true;
}

void WiFiService::forgetAll() {
  memset(&g_creds, 0, sizeof(g_creds));
  saveCreds();
  NvsStore::clearNamespace(nvsns::kWifi);
  SH_LOGW(TAG, "all networks forgotten");
}

uint8_t WiFiService::savedNetworkCount() { return g_creds.count; }

size_t WiFiService::savedNetworksToJson(char* out, size_t cap) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < g_creds.count; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = g_creds.nets[i].ssid;
    o["hasPassword"] = g_creds.nets[i].pass[0] != '\0';
  }
  return serializeJson(doc, out, cap);
}

size_t WiFiService::scanToJson(char* out, size_t cap) {
  // Called from the HTTP handler: returns the cache immediately and asks the
  // Wi-Fi task to refresh if the data is stale. Never blocks.
  const bool stale = g_scanAtMs == 0 || (millis() - g_scanAtMs) > 30000;
  if (stale && !g_scanRunning) g_scanRequested = true;

  JsonDocument doc;
  doc["scanning"] = g_scanRunning || g_scanRequested;
  doc["ageMs"] = g_scanAtMs ? (millis() - g_scanAtMs) : 0;
  JsonArray arr = doc["networks"].to<JsonArray>();
  const uint8_t n = g_scanCount;
  for (uint8_t i = 0; i < n; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = g_scan[i].ssid;
    o["rssi"] = g_scan[i].rssi;
    o["channel"] = g_scan[i].channel;
    o["secure"] = g_scan[i].secure;
    o["known"] = g_scan[i].known;
  }
  return serializeJson(doc, out, cap);
}

void WiFiService::requestScan() { g_scanRequested = true; }
bool WiFiService::scanInProgress() { return g_scanRunning || g_scanRequested; }

void WiFiService::requestReconnect() { g_reconnectRequested = true; }

void WiFiService::startAp() {
  if (g_apActive) return;
  // AP+STA so the device keeps trying to rejoin the real network while the
  // portal is up. A recovery AP that gives up on the station is a trap.
  WiFi.mode(WIFI_AP_STA);
  const bool ok = WiFi.softAP(g_apSsid, g_apPass, /*channel=*/1,
                              /*ssid_hidden=*/0, /*max_connection=*/4);
  if (!ok) {
    SH_LOGE(TAG, "softAP start failed");
    return;
  }
  g_apActive = true;
  SH_LOGW(TAG, "provisioning AP up: ssid=%s pass=%s ip=%s", g_apSsid, g_apPass,
          WiFi.softAPIP().toString().c_str());
}

void WiFiService::stopAp() {
  if (!g_apActive) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  g_apActive = false;
  SH_LOGI(TAG, "provisioning AP stopped");
}

}  // namespace sh
