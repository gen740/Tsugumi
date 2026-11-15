// Safe transfer and removal of private worker staging, never run-result
// removal.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>

namespace tsugumi::fs {

// Empty an existing directory without replacing it (it may be mounted in a
// resident worker). Follow no symlinks. Restore owner permissions on opened
// directories, since Cards can copy read-only trees from the Nix store.
// Throws std::system_error with the operation, path and errno on failure.
// Darwin cannot pin an unreadable directory. A live worker's owner supplies
// recover(parent, name), executed with the Card's own identity and sandbox.
// Without a callback, unreadable Darwin directories fail with EACCES.
auto clear_staging(const std::filesystem::path& path,
                   const std::function<void(int, const char*)>& recover = {}) -> void;

// Copy only one declared File/Directory from a pinned staging root. Within a
// Directory, materialize relative symlinks whose fully resolved targets stay
// inside that output. No kernel lookup follows a symlink, including during
// concurrent replacement. The destination must be private to the core.
// Throws with the offending path and entry kind; cancellation is also checked.
auto copy_staging_output(int staging, const std::filesystem::path& relative,
                         const std::filesystem::path& destination,
                         bool directory, const std::atomic<bool>& cancel)
    -> void;

}  // namespace tsugumi::fs
