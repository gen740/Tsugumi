#pragma once

#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace tsugumi::host {
// One version for all persistent host state. Raise it and update the digest
// when changing the tables covered by tests/static/test_host_state_format.py.
inline constexpr int kStateFormat = 2;
inline constexpr std::string_view kStateSchemaDigest =
    "6ac5eb16017ec3b070e15212bf705d1e02c0573196f7ad4e965615b61fd1b5f5";

class StateFormatError final : public std::runtime_error {
 public:
  explicit StateFormatError(const std::filesystem::path& directory);
};

// Only a newly created directory receives a format marker. Existing state is
// checked before any database is opened; it is never migrated or removed.
auto PrepareStateDirectory(const std::filesystem::path& directory) -> void;
}  // namespace tsugumi::host
