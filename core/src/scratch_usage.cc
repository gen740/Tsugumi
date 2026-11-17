#include "scratch_usage.hh"
#include <algorithm>
namespace tsugumi {
auto measure_run_scratch(const std::filesystem::path& root, const std::function<bool()>& cancelled)
    -> std::vector<RunScratchUsage> {
  namespace fs = std::filesystem;
  std::vector<RunScratchUsage> result;
  bool stop = false;
  const auto stopped = [&] { return stop || (cancelled && (stop = cancelled())); };
  const auto measure = [&](const fs::path& path) {
    uint64_t bytes = 0;
    RunScratchUsage usage{path.lexically_relative(root).generic_string(), std::nullopt};
    try {
      if (fs::is_directory(fs::symlink_status(path)))
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
          if (stopped()) return;
          if (fs::is_regular_file(entry.symlink_status())) bytes += entry.file_size();
        }
      usage.bytes = bytes;
    } catch (const fs::filesystem_error&) {
      // Keep this folder selectable even when a Card left it unreadable.
    }
    result.push_back(std::move(usage));
  };
  if (fs::is_directory(fs::symlink_status(root / "runs")))
    for (const auto& entry : fs::directory_iterator(root / "runs")) {
      if (stopped()) return result;
      measure(entry.path());
    }
  if (stopped()) return result;
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
  return result;
}

}  // namespace tsugumi
