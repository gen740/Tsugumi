#pragma once

#include <filesystem>

namespace tsugumi {

// Creates a directory owned by this user with no access for other users.
// Refuses links, someone else's directory and memory-backed Linux filesystems.
auto private_disk_scratch(const std::filesystem::path& directory) -> std::filesystem::path;
auto default_run_scratch(const std::filesystem::path& project) -> std::filesystem::path;

}  // namespace tsugumi
