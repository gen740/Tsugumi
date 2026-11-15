#pragma once

#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace tsugumi::host {
// One version for all persistent host state. Raise it and update the digest
// when changing the tables covered by tests/static/test_host_state_format.py.
inline constexpr int kStateFormat = 5;
inline constexpr std::string_view kStateSchemaDigest =
    "8384e45caf6601a09b68f91fd00556d45ab248e5413cf598c7d9218d29194e27";

class StateFormatError final : public std::runtime_error {
 public:
  explicit StateFormatError(const std::filesystem::path& directory);
};

// Only a newly created directory receives a format marker. Existing state is
// checked before any database is opened; it is never migrated or removed.
auto PrepareStateDirectory(const std::filesystem::path& directory) -> void;
}  // namespace tsugumi::host
