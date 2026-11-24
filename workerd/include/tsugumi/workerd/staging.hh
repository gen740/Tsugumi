// Safe transfer and removal of private worker staging, never run-result
// removal.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>

namespace tsugumi::fs {
// Empty an existing directory without replacing it (it may be mounted in a
// resident worker). Follow no symlinks. Restore owner permissions on opened
// directories, since Cards can copy read-only trees from the Nix store.
// Throws std::system_error with the operation, path and errno on failure.
// Darwin cannot pin an unreadable directory. A live worker's owner supplies
// recover(parent, name), executed with the Card's own identity and sandbox.
// Without a callback, unreadable Darwin directories fail with EACCES.
auto clear_staging(const std::filesystem::path& path,
                   const std::function<void(int, const char*)>& recover = {},
                   std::string_view preserved_entry = {}) -> void;

// Limits count logical bytes, including sparse holes, and filesystem entries
// across a call's new outputs. A forwarded input never consumes this budget.
struct OutputBudget {
  uint64_t bytes = 64ull << 30;
  uint64_t entries = 1000;
};

// Clone a new artifact directory from the pinned staging directory. Each file
// of the clone is a new inode, so a writer that outlived the call cannot
// change it. macOS uses clonefile(2), which shares blocks copy-on-write and
// needs the destination on the same APFS volume; another filesystem fails the
// transfer. Linux uses a reflink (FICLONE) where the filesystem supports one
// and copies the file otherwise. The artifact name is a single component, and
// links inside it are rejected. The budget is checked, not consumed.
auto clone_staging_artifact(int staging, const std::filesystem::path& name,
                            const std::filesystem::path& destination,
                            OutputBudget budget) -> void;

// Remove an artifact directory from the pinned staging directory without
// following links. Throws std::system_error like clear_staging.
auto remove_staging_artifact(int staging, const std::filesystem::path& name)
    -> void;

// Materialize the granted tree in a directory writable only by the supervisor.
auto link_artifact(const std::filesystem::path& source,
                   const std::filesystem::path& destination) -> void;

// Validate and move an artifact after the process that wrote it is retired.
auto move_staging_artifact(int staging, const std::filesystem::path& name,
                           const std::filesystem::path& destination,
                           OutputBudget& budget) -> void;

}  // namespace tsugumi::fs
