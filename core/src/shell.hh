#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace tsugumi::shell {
struct Output {
  std::array<std::string, 2> streams;
  std::array<bool, 2> truncated{};
  int status = 0;
  bool cancelled = false;
  bool timed_out = false;
};

auto Execute(const std::string& command, const std::string& cwd,
             uint32_t timeout, const std::atomic<bool>& cancelled,
             bool read_only, bool hide_credentials, bool no_project_socket,
             const std::filesystem::path& control) -> Output;
}  // namespace tsugumi::shell
