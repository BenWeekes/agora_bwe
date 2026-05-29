#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// Lightweight structured logging to stderr (human-readable, timestamped) plus an
// optional append-only stats file used to reconstruct every ABR decision offline.
namespace bwe {

enum class LogLevel { INFO, WARN, DECISION, ERROR };

// Millisecond-precision wall-clock timestamp, e.g. "2026-05-29 21:30:42.123".
std::string now_timestamp();

// Monotonic milliseconds since an arbitrary fixed epoch (steady clock). Used for
// all ABR timing so wall-clock adjustments can't perturb the controller.
uint64_t mono_ms();

// Second-precision wall-clock timestamp, e.g. "2026-05-29 21:30:42".
std::string now_timestamp_secs();

// Open the append-only stats file. Pass an empty path to disable file logging.
void set_log_file(const std::string& path);
void close_log_file();

// Open the unified network log. Every SDK network callback writes one line here,
// each tagged by perspective (LOCAL = this publisher, REMOTE = another user) and
// direction (uplink = sending, downlink = receiving). `tail -f` to watch live.
// Pass an empty path to disable. See publisher_observer.h for the line formats.
void set_net_log_file(const std::string& path);
void log_net(const std::string& line);

// stderr logging. printf-style.
void log_line(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Append a raw line to the stats file (no level prefix, caller-formatted). The line
// is also given a timestamp prefix. No-op if no stats file is configured.
void log_stats(const std::string& line);

}  // namespace bwe

#define LOG_INFO(...) ::bwe::log_line(::bwe::LogLevel::INFO, __VA_ARGS__)
#define LOG_WARN(...) ::bwe::log_line(::bwe::LogLevel::WARN, __VA_ARGS__)
#define LOG_DECISION(...) ::bwe::log_line(::bwe::LogLevel::DECISION, __VA_ARGS__)
#define LOG_ERROR(...) ::bwe::log_line(::bwe::LogLevel::ERROR, __VA_ARGS__)
