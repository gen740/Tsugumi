// Removal of private worker scratch, never of retained run results.
#pragma once

#include <filesystem>

namespace tsugumi::fs {

// Empty an existing directory without replacing it (it may be mounted in a
// resident worker). Follow no symlinks. Restore owner permissions on opened
// directories, since Cards can copy read-only trees from the Nix store.
// Throws std::system_error with the operation, path and errno on failure.
auto clear_staging(const std::filesystem::path& path) -> void;

}  // namespace tsugumi::fs
