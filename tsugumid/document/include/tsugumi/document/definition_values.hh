// Definition values shared with workers.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace tsugumi {
using CardPath = std::vector<std::string>;
auto path_key(const CardPath& path) -> std::string;
// Zero represents an absent limit. CPU quotas are in thousandths of a core.
struct ExecutionResources {
  uint32_t cpu_millis = 0;
  uint64_t memory_bytes = 0;
  auto limited() const -> bool { return cpu_millis || memory_bytes; }
};
struct SandboxOptions {
  bool filesystem = true;
  bool pid = true;
  bool ipc = true;
  bool identity = true;
  ExecutionResources limits;
};

inline auto grant_environment(std::string_view name) -> std::string {
  std::string key = "TSUGUMI_GRANT_";
  for (char c : name) {
    if (c >= 'a' && c <= 'z') c += 'A' - 'a';
    key += (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_';
  }
  return key;
}
}  // namespace tsugumi
