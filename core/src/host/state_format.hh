#pragma once

#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace tsugumi::host {
// One version for all persistent host state. Raise it and update the digest
// when changing the tables covered by tests/static/test_host_state_format.py.
inline constexpr int kStateFormat = 6;
inline constexpr std::string_view kStateSchemaDigest =
    "f29075aada380aa9b9c98f582b73d1db3b3431f5f42b15228122a7b89fe0d663";

class StateFormatError final : public std::runtime_error {
 public:
  const std::filesystem::path directory;
  explicit StateFormatError(const std::filesystem::path& directory);
};

// Only a newly created directory receives a format marker. Existing state is
// checked before any database is opened; it is never migrated or removed.
auto PrepareStateDirectory(const std::filesystem::path& directory) -> void;
}  // namespace tsugumi::host
