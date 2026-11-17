#pragma once

#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace tsugumi::host {
// One version for all persistent host state. Raise it and update the digest
// when changing the tables covered by tests/static/test_host_state_format.py.
inline constexpr int kStateFormat = 7;
inline constexpr std::string_view kStateSchemaDigest =
    "40b14a8aeb93cbf36f9f7d1b15731b1fd1b5ecdbec2383ef542527aa763ca019";

class StateFormatError final : public std::runtime_error {
 public:
  const std::filesystem::path directory;
  explicit StateFormatError(const std::filesystem::path& directory);
};

// Only a newly created directory receives a format marker. Existing state is
// checked before any database is opened; it is never migrated or removed.
auto PrepareStateDirectory(const std::filesystem::path& directory) -> void;
}  // namespace tsugumi::host
