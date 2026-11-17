// Definition values shared with workers.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace tsugumi {
using CardPath = std::vector<std::string>;
auto path_key(const CardPath& path) -> std::string;
struct ExecutionResources {
  uint32_t cpu_millis;
  uint64_t memory_bytes;
};
}  // namespace tsugumi
