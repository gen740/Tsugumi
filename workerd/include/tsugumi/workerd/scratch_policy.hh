// Shared launch policy, without file utility or sampler ownership.
#pragma once
#include <filesystem>
namespace tsugumi {
auto run_scratch_root() -> std::filesystem::path;
}  // namespace tsugumi
