#include "log/Logger.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <string.h>

namespace sh {
namespace {

constexpr size_t kLineMax = 160;
constexpr size_t kRingLines = 24;   // ~3.8 KB of RAM, recent history for /diag
constexpr size_t kRingLineMax = 160;

LogLevel g_level = LogLevel::Info;
Logger::RemoteSink g_remote = nullptr;
SemaphoreHandle_t g_mutex = nullptr;

char g_ring[kRingLines][kRingLineMax];
uint8_t g_ringHead = 0;   // next slot to write
uint8_t g_ringCount = 0;

struct Lock {
  bool taken = false;
  explicit Lock(TickType_t wait) {
    if (g_mutex) taken = xSemaphoreTake(g_mutex, wait) == pdTRUE;
  }
  ~Lock() {
    if (taken) xSemaphoreGive(g_mutex);
  }
};

}  // namespace

void Logger::begin(uint32_t baud, LogLevel level) {
  if (!g_mutex) g_mutex = xSemaphoreCreateMutex();
  Serial.begin(baud);
  g_level = level;
  memset(g_ring, 0, sizeof(g_ring));
  g_ringHead = 0;
  g_ringCount = 0;
}

void Logger::setLevel(LogLevel level) { g_level = level; }
LogLevel Logger::level() { return g_level; }
void Logger::setRemoteSink(RemoteSink sink) { g_remote = sink; }

const char* Logger::levelName(LogLevel level) {
  switch (level) {
    case LogLevel::Error:   return "E";
    case LogLevel::Warn:    return "W";
    case LogLevel::Info:    return "I";
    case LogLevel::Debug:   return "D";
    case LogLevel::Verbose: return "V";
    default:                return "-";
  }
}

void Logger::pushRing(const char* line) {
  strncpy(g_ring[g_ringHead], line, kRingLineMax - 1);
  g_ring[g_ringHead][kRingLineMax - 1] = '\0';
  g_ringHead = static_cast<uint8_t>((g_ringHead + 1) % kRingLines);
  if (g_ringCount < kRingLines) g_ringCount++;
}

void Logger::vlog(LogLevel level, const char* tag, const char* fmt, va_list ap) {
  if (level > g_level || level == LogLevel::None) return;

  char body[kLineMax];
  vsnprintf(body, sizeof(body), fmt, ap);

  char line[kLineMax + 48];
  snprintf(line, sizeof(line), "[%8lu][%s][%s] %s",
           static_cast<unsigned long>(millis()), levelName(level),
           tag ? tag : "?", body);

  // Short wait: logging must never deadlock the caller. If the mutex is busy
  // we still print - interleaved output beats a stalled task.
  {
    Lock lock(pdMS_TO_TICKS(20));
    Serial.println(line);
    pushRing(line);
  }

  RemoteSink sink = g_remote;
  if (sink && level <= LogLevel::Warn) sink(level, tag, body);
}

void Logger::log(LogLevel level, const char* tag, const char* fmt, ...) {
  if (level > g_level || level == LogLevel::None) return;
  va_list ap;
  va_start(ap, fmt);
  vlog(level, tag, fmt, ap);
  va_end(ap);
}

size_t Logger::dumpRing(char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  Lock lock(pdMS_TO_TICKS(50));
  size_t used = 0;
  out[0] = '\0';

  const uint8_t start =
      (g_ringCount < kRingLines) ? 0 : g_ringHead;  // oldest entry
  for (uint8_t i = 0; i < g_ringCount; ++i) {
    const uint8_t idx = static_cast<uint8_t>((start + i) % kRingLines);
    const size_t len = strlen(g_ring[idx]);
    if (used + len + 2 >= cap) break;
    memcpy(out + used, g_ring[idx], len);
    used += len;
    out[used++] = '\n';
  }
  out[used] = '\0';
  return used;
}

}  // namespace sh
