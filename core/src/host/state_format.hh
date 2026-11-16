#pragma once

#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace tsugumi::host {
// One version for all persistent host state. Raise it and update the digest
// when changing the tables covered by tests/static/test_host_state_format.py.
inline constexpr int kStateFormat = 4;
inline constexpr std::string_view kStateSchemaDigest =
    "5b88e187206b7f8cd16864ad7eb6ef555ab47e108d08b6ec988f6057dc5d3c26";

class StateFormatError final : public std::runtime_error {
 public:
  const std::filesystem::path directory;
  explicit StateFormatError(const std::filesystem::path& directory);
};

// Only a newly created directory receives a format marker. Existing state is
// checked before any database is opened; it is never migrated or removed.
auto PrepareStateDirectory(const std::filesystem::path& directory) -> void;
}  // namespace tsugumi::host
