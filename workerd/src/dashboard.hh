// The worker's dashboard: an HTTPS page on its own port. Anyone who reaches
// the port sees the worker's state. A logged-in administrator can change it.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "tsugumi/rpc/dashboard_http.hh"
#include "worker_settings.hh"

namespace tsugumi::workerd {

class Dashboard {
 public:
  using Clock = std::chrono::steady_clock;
  using Wall = std::chrono::system_clock;
  // What the page shows of the listener, and what it asks of it.
  struct Listener {
    std::string system;
    // The source revision the listener was built from; empty when unknown.
    std::string revision;
    // Jobs whose supervisors run now.
    std::function<uint32_t()> running;
    // Restart once no job runs, or now, stopping the running jobs.
    std::function<void(bool now)> restart;
    std::function<bool()> restart_pending;
    // When each host key last connected since the listener started.
    std::function<std::map<std::string, Wall::time_point>()> last_seen;
  };
  Dashboard(std::filesystem::path root, WorkerSettings& settings,
            Listener listener);
  ~Dashboard();
  // Set once after binding, before the first connection is served.
  auto listening(uint16_t port, uint16_t dashboard_port) -> void {
    port_ = port;
    dashboard_port_ = dashboard_port;
  }
  auto serve(peer_tls::Stream& stream) -> void;

 private:
  struct Session {
    std::string csrf;
    Clock::time_point expires;
  };
  auto handle(const dashboard_http::Request& request) -> dashboard_http::Reply;
  auto session(const dashboard_http::Request& request) -> const Session*;
  // Who asks, read under mutex_; the state itself is built without it.
  struct Viewer {
    bool admin = false;
    std::string csrf;
    uint64_t locked_seconds = 0;
  };
  auto viewer(const dashboard_http::Request& request) -> Viewer;
  auto state(const Viewer& viewer) -> std::string;
  auto login(std::string_view password) -> dashboard_http::Reply;
  // Called with mutex_ held so verification and its effects are serialized.
  auto verify_password(std::string_view password)
      -> std::optional<dashboard_http::Reply>;
  auto change_password(const Session& admin, std::string_view current,
                       std::string_view next, std::string_view confirmation)
      -> dashboard_http::Reply;
  auto locked_seconds() -> uint64_t;
  auto edit_key(std::string_view key, bool add) -> void;
  // The last measured store and job bytes. Walking a large store takes tens
  // of seconds, so a background thread measures them and the page and logins
  // never wait for it.
  struct Measured {
    uint64_t store = 0, jobs = 0;
    bool done = false;
  };
  auto measured() -> Measured;

  std::filesystem::path root_, keys_;
  WorkerSettings& settings_;
  Listener listener_;
  Clock::time_point began_ = Clock::now();
  uint16_t port_ = 0, dashboard_port_ = 0;
  std::mutex mutex_;
  std::map<std::string, Session> sessions_;
  unsigned failures_ = 0;
  Clock::time_point locked_until_{};
  std::mutex disk_mutex_;
  Clock::time_point disk_at_{};
  Measured disk_{};
  bool walking_ = false;
  std::atomic<bool> stopping_{false};
  std::thread walker_;
};

}  // namespace tsugumi::workerd
