#pragma once

#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace tsugumi::host {
// One version for all persistent host state. Raise it and update the digest
// when changing the tables covered by tests/static/test_host_state_format.py.
inline constexpr int kStateFormat = 1;
inline constexpr std::string_view kStateSchemaDigest =
    "05fcf7bc650824b5d076bc7ead171c2f7beff93c54d2c9d3ea707f8d0d5b673a";

class StateFormatError final : public std::runtime_error {
 public:
  explicit StateFormatError(const std::filesystem::path& directory);
};

// Only a newly created directory receives a format marker. Existing state is
// checked before any database is opened; it is never migrated or removed.
auto PrepareStateDirectory(const std::filesystem::path& directory) -> void;
}  // namespace tsugumi::host
