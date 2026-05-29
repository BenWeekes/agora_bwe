#include "logging.h"

#include <cstdarg>
#include <chrono>
#include <ctime>
#include <mutex>

namespace bwe {

namespace {
std::mutex g_mutex;
FILE* g_stats_file = nullptr;
FILE* g_net_file = nullptr;

const char* level_name(LogLevel level) {
  switch (level) {
    case LogLevel::INFO: return "INFO";
    case LogLevel::WARN: return "WARN";
    case LogLevel::DECISION: return "DECISION";
    case LogLevel::ERROR: return "ERROR";
  }
  return "?";
}
}  // namespace

std::string now_timestamp() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  std::time_t t = system_clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                tm.tm_sec, static_cast<int>(ms.count()));
  return std::string(buf);
}

std::string now_timestamp_secs() {
  using namespace std::chrono;
  std::time_t t = system_clock::to_time_t(system_clock::now());
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  return std::string(buf);
}

uint64_t mono_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void set_log_file(const std::string& path) {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_stats_file) {
    fclose(g_stats_file);
    g_stats_file = nullptr;
  }
  if (!path.empty()) {
    g_stats_file = fopen(path.c_str(), "w");  // truncate: clear old log each run
    if (!g_stats_file) {
      fprintf(stderr, "[%s] WARN failed to open log file %s\n", now_timestamp().c_str(),
              path.c_str());
    }
  }
}

void set_net_log_file(const std::string& path) {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_net_file) {
    fclose(g_net_file);
    g_net_file = nullptr;
  }
  if (!path.empty()) {
    g_net_file = fopen(path.c_str(), "w");  // truncate: clear old log each run
    if (!g_net_file) {
      fprintf(stderr, "[%s] WARN failed to open net log file %s\n", now_timestamp().c_str(),
              path.c_str());
    }
  }
}

void log_net(const std::string& line) {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (!g_net_file) return;
  fprintf(g_net_file, "[%s] %s\n", now_timestamp_secs().c_str(), line.c_str());
  fflush(g_net_file);
}

void close_log_file() {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_stats_file) {
    fclose(g_stats_file);
    g_stats_file = nullptr;
  }
  if (g_net_file) {
    fclose(g_net_file);
    g_net_file = nullptr;
  }
}

void log_line(LogLevel level, const char* fmt, ...) {
  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  std::lock_guard<std::mutex> lk(g_mutex);
  std::string ts = now_timestamp();
  fprintf(stderr, "[%s] %s %s\n", ts.c_str(), level_name(level), msg);
  // DECISION lines are the durable record of why a switch happened — mirror them
  // into the stats file too.
  if (g_stats_file && level == LogLevel::DECISION) {
    fprintf(g_stats_file, "[%s] %s %s\n", ts.c_str(), level_name(level), msg);
    fflush(g_stats_file);
  }
}

void log_stats(const std::string& line) {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (!g_stats_file) return;
  fprintf(g_stats_file, "[%s] %s\n", now_timestamp().c_str(), line.c_str());
  fflush(g_stats_file);
}

}  // namespace bwe
