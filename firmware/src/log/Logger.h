// ---------------------------------------------------------------------------
//  Logger.h - levelled logging with a serial sink, an in-RAM ring buffer for
//  diagnostics, and an optional remote sink (MQTT) installed at runtime.
//
//  Contract: log() may be called from any task. It may NOT be called from an
//  ISR. The remote sink must not block - it is invoked inline.
// ---------------------------------------------------------------------------
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifndef SH_LOG_LEVEL
#define SH_LOG_LEVEL 4
#endif

namespace sh {

enum class LogLevel : uint8_t {
  None = 0, Error = 1, Warn = 2, Info = 3, Debug = 4, Verbose = 5
};

class Logger {
 public:
  using RemoteSink = void (*)(LogLevel level, const char* tag, const char* line);

  static void begin(uint32_t baud, LogLevel level);
  static void setLevel(LogLevel level);
  static LogLevel level();

  static void log(LogLevel level, const char* tag, const char* fmt, ...);
  static void vlog(LogLevel level, const char* tag, const char* fmt, va_list ap);

  /// Installed by MqttService once connected; cleared on disconnect.
  static void setRemoteSink(RemoteSink sink);

  /// Copies the most recent lines (oldest first) into `out`, NUL-terminated.
  /// Used by the diagnostics endpoint so you can see why a device misbehaved
  /// without a serial cable.
  static size_t dumpRing(char* out, size_t cap);

  static const char* levelName(LogLevel level);

 private:
  static void pushRing(const char* line);
};

}  // namespace sh

// Compile-time elimination: anything above SH_LOG_LEVEL costs zero bytes.
#if SH_LOG_LEVEL >= 1
#define SH_LOGE(tag, ...) ::sh::Logger::log(::sh::LogLevel::Error, tag, __VA_ARGS__)
#else
#define SH_LOGE(tag, ...) do {} while (0)
#endif
#if SH_LOG_LEVEL >= 2
#define SH_LOGW(tag, ...) ::sh::Logger::log(::sh::LogLevel::Warn, tag, __VA_ARGS__)
#else
#define SH_LOGW(tag, ...) do {} while (0)
#endif
#if SH_LOG_LEVEL >= 3
#define SH_LOGI(tag, ...) ::sh::Logger::log(::sh::LogLevel::Info, tag, __VA_ARGS__)
#else
#define SH_LOGI(tag, ...) do {} while (0)
#endif
#if SH_LOG_LEVEL >= 4
#define SH_LOGD(tag, ...) ::sh::Logger::log(::sh::LogLevel::Debug, tag, __VA_ARGS__)
#else
#define SH_LOGD(tag, ...) do {} while (0)
#endif
#if SH_LOG_LEVEL >= 5
#define SH_LOGV(tag, ...) ::sh::Logger::log(::sh::LogLevel::Verbose, tag, __VA_ARGS__)
#else
#define SH_LOGV(tag, ...) do {} while (0)
#endif
