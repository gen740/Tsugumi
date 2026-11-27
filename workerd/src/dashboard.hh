#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include "tsugumi/rpc/dashboard_http.hh"

namespace tsugumi::workerd {

class Dashboard {
 public:
  Dashboard(std::filesystem::path root, std::filesystem::path keys,
            std::string server);
  auto serve(peer_tls::Stream& stream, std::string prefix) -> void;

 private:
  using Clock = std::chrono::steady_clock;
  struct Challenge {
    std::string peer;
    Clock::time_point expires;
  };

  auto handle(dashboard_http::Request request, std::string peer)
      -> dashboard_http::Reply;
  auto issue(std::string peer) -> std::string;
  auto consume(const std::string& token, std::string_view peer) -> bool;
  auto edit_key(std::string_view key, bool add) -> void;

  std::filesystem::path root_, keys_;
  std::string server_;
  std::mutex challenge_mutex_;
  std::map<std::string, Challenge> challenges_;
};

}  // namespace tsugumi::workerd
