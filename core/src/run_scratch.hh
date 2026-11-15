#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tsugumi {

// Creates a directory owned by this user with no access for other users.
// Refuses links, someone else's directory and memory-backed Linux filesystems.
auto private_disk_scratch(const std::filesystem::path& directory) -> std::filesystem::path;
// Validates the same constraints without creating missing directories.
auto disk_scratch_path(const std::filesystem::path& directory) -> std::filesystem::path;
auto default_run_scratch(const std::filesystem::path& project) -> std::filesystem::path;

struct RunScratchUsage {
  std::string path;
  std::optional<uint64_t> bytes;
};
// Measures without following symbolic links. Errors leave the measurement unknown.
// `cancelled` is asked before every entry; once it answers true the walk ends
// and the partial result is the caller's to discard.
auto measure_run_scratch(const std::filesystem::path& root, const std::function<bool()>& cancelled = {})
    -> std::vector<RunScratchUsage>;
// Removes read-only Card output too, without following symbolic links.
// Returns false if anything remains, so the UI can report retained folders.
auto remove_run_scratch(const std::filesystem::path& path) -> bool;

}  // namespace tsugumi
