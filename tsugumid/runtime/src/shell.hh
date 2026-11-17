#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tsugumi::shell {
struct Output {
  std::array<std::string, 2> streams;
  std::array<bool, 2> truncated{};
  std::array<uint64_t, 2> sizes{};
  int status = 0;
  bool cancelled = false;
  bool timed_out = false;
};

struct Mount {
  std::filesystem::path source, target;
  bool writable = false;
};
struct View {
  std::filesystem::path hidden_root;
  std::vector<std::filesystem::path> writable, protected_files, excluded;
  std::vector<Mount> mounts;
  std::filesystem::path output_prefix;
};

auto Execute(const std::string& command, const std::string& cwd,
             uint32_t timeout, const std::atomic<bool>& cancelled,
             bool read_only, bool hide_credentials, bool no_project_socket,
             const std::filesystem::path& control, const View& view = {})
    -> Output;
}  // namespace tsugumi::shell
