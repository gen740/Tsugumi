// Store metadata and process initialization without the evaluator.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace tsugumi {
class NixStore {
 public:
  static auto init_process() -> void;
  // The store directory (normally /nix/store), the boundary of served files.
  static auto store_dir() -> std::string;
  static auto substituters() -> std::vector<std::string>;
  static auto closure(const std::string& path) -> std::vector<std::string>;
  static auto rooted_usage(const std::vector<std::vector<std::string>>& roots)
      -> std::vector<std::map<std::string, uint64_t>>;
};
}  // namespace tsugumi
