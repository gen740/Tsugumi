#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "server.hh"
#include "util/sqlite.hh"

namespace tsugumi::host {
class Settings;
struct Models {
  Models(sql::Db& db, Settings& settings, std::stop_token stop,
         std::function<std::string(std::string)> scrub);
  static auto StoredChatGPTModels(const std::filesystem::path& state_dir)
      -> std::vector<std::string>;
  static auto ContextWindowTokens(const std::filesystem::path& state_dir,
                                  std::string_view provider,
                                  std::string_view model) -> uint32_t;
  auto Handle(const RequestIn& request, bool human,
              std::unique_lock<std::recursive_mutex>& lock) -> std::string;
  sql::Db& db;
  Settings* host_settings;
  std::stop_token stop;
  std::function<std::string(std::string)> scrub;
};
}  // namespace tsugumi::host
