#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace tsugumi::host {
// Host-owned settings and the immutable configuration supplied at guest boot.
struct BinaryCacheSnapshot {
  std::string configuration;
  std::string netrc;
  std::string problem;
};
auto ValidateBinaryCaches(std::string_view json) -> void;
auto LoadBinaryCaches(const std::filesystem::path &support) -> std::string;
auto SaveBinaryCaches(const std::filesystem::path &support,
                      std::string_view json) -> void;
auto PrepareBinaryCaches(const std::filesystem::path &support)
    -> BinaryCacheSnapshot;
} // namespace tsugumi::host
