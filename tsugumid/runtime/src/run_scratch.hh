#pragma once
#include "tsugumi/runtime_support/scratch_usage.hh"

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

auto remove_run_scratch(const std::filesystem::path& path) -> bool;

}  // namespace tsugumi
