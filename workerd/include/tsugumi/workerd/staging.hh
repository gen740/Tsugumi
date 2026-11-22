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

// Snapshot one Path output, a regular file or a whole directory as found, from
// a pinned working directory into private storage. Resolve no symlinks to
// outside the output and accept only regular files and directories.
auto copy_staging_output(int staging, const std::filesystem::path& relative,
                         const std::filesystem::path& destination,
                         const std::atomic<bool>& cancel,
                         const TransferProgress& progress = {},
                         OutputBudget* budget = nullptr) -> void;

}  // namespace tsugumi::fs
