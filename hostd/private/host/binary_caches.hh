#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tsugumi::host {
// Host-owned settings and the immutable configuration supplied at guest boot.
struct BinaryCacheSnapshot {
  std::string configuration;
  std::string netrc;
  std::string problem;
};
struct BinaryCachePull {
  std::string endpoint, public_key, host, user, secret;
};
auto LoadBinaryCachePulls(const std::filesystem::path& support)
    -> std::vector<BinaryCachePull>;
auto LoadBinaryCaches(const std::filesystem::path& support) -> std::string;
auto SaveBinaryCaches(const std::filesystem::path& support,
                      std::string_view json) -> void;
auto PrepareBinaryCaches(const std::filesystem::path& support)
    -> BinaryCacheSnapshot;
}  // namespace tsugumi::host
