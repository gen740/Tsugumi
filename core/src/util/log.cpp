#include "util/log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace tsugumi::log {

namespace {
std::mutex mu;

void write(const char* level, const std::string& msg) {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
  gmtime_r(&t, &tm);
  char ts[32];
  std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
  std::lock_guard<std::mutex> l(mu);
  std::fprintf(stderr, "%s.%03lldZ tsugumid %s %s\n", ts, static_cast<long long>(ms), level, msg.c_str());
}
}  // namespace

void info(const std::string& msg) { write("info", msg); }
void warn(const std::string& msg) { write("warn", msg); }
void error(const std::string& msg) { write("error", msg); }

}  // namespace tsugumi::log
