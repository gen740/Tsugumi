// Safe transfer and removal of private worker staging, never run-result
// removal.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>

namespace tsugumi::fs {
using TransferProgress =
    std::function<void(const std::filesystem::path&, uint64_t, uint64_t)>;

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

// Copy a new artifact directory from the pinned staging directory. The
// artifact name is a single component and links inside it are rejected.
auto copy_staging_artifact(int staging, const std::filesystem::path& name,
                           const std::filesystem::path& destination,
                           const std::atomic<bool>& cancel, OutputBudget& budget,
                           const TransferProgress& progress = {}) -> void;

// Materialize the granted tree in a directory writable only by the supervisor.
auto link_artifact(const std::filesystem::path& source,
                   const std::filesystem::path& destination) -> void;

// Validate and move an artifact after the process that wrote it is retired.
auto move_staging_artifact(int staging, const std::filesystem::path& name,
                           const std::filesystem::path& destination,
                           OutputBudget& budget) -> void;

}  // namespace tsugumi::fs
