#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace tsugumi {
struct RunScratchUsage {
  std::string path;
  std::optional<uint64_t> bytes;
};
// Measures without following symbolic links. Errors leave the measurement unknown.
// `cancelled` is asked before every entry; once it answers true the walk ends
// and the partial result is the caller's to discard.
auto measure_run_scratch(const std::filesystem::path& root, const std::function<bool()>& cancelled = {})
    -> std::vector<RunScratchUsage>;
}  // namespace tsugumi
