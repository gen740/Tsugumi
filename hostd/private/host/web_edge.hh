// The browser's origin: hostd serves the Web UI's files, exchanges the page
// key for a cookie and hands authenticated WebSocket upgrades to the Host and
// Agent RPC servers.
#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>

#include "tsugumi/rpc/transport.hh"

namespace tsugumi::host {

struct WebEdgeOptions {
  std::filesystem::path root;  // the built page (index.html, modules/, ...)
  int port = 8780;
  // A port that was asked for fails when taken; the usual one falls back to
  // any free port (the page's localStorage is per origin).
  bool port_chosen = false;
  std::string key;         // the page key, exchanged once for the cookie
  std::string recovery;
  std::string recovery_token;
  std::function<void()> move_state_aside;
  std::function<void()> state_moved;
  std::string user_token;  // the Host Hello token the page's cookie unlocks
};

class WebEdge {
 public:
  WebEdge(WebEdgeOptions options, TransportServer* host, TransportServer* agent);
  ~WebEdge();
  WebEdge(const WebEdge&) = delete;
  auto operator=(const WebEdge&) -> WebEdge& = delete;

  auto start() -> void;
  auto stop() -> void;
  auto port() const -> int { return port_; }
  /** The URL that opens a page, carrying the key. */
  auto page_url(const std::string& project = {}) const -> std::string;

 private:
  auto loop() -> void;
  auto serve(int fd) -> void;
  auto Release(int fd) -> void;  // closes a connection this edge still owns

  static constexpr size_t kMaxConnections = 64;

  WebEdgeOptions options_;
  TransportServer* host_;
  TransportServer* agent_;
  int listen_fd_ = -1;
  int wake_r_ = -1;
  int wake_w_ = -1;
  int port_ = -1;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  std::mutex connections_mutex_;
  std::condition_variable drained_;
  std::set<int> connections_;
  size_t active_ = 0;
};

/** Percent-encodes a URL query value. */
auto UrlEncode(std::string_view value) -> std::string;

}  // namespace tsugumi::host
