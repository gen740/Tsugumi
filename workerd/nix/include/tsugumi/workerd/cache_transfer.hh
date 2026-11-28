// Which store paths cross to and from Tsugumi's binary caches. A path that a
// pull cache (the store's configured substituters: cache.nixos.org, the
// project's pull caches, Attic) serves never goes through Tsugumi: a worker
// substitutes it from there itself, and nothing uploads it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace nix {
class Store;
}

namespace tsugumi {

// A transfer's status: `done` of `total` paths and of their NAR bytes, and
// the path now in flight.
struct CacheTransferProgress {
  size_t done = 0;
  size_t total = 0;
  uint64_t bytes_done = 0;
  uint64_t bytes_total = 0;
  std::string path;
};
using CacheTransferReport = std::function<void(const CacheTransferProgress&)>;

// The part of a closure to upload, references first.
struct CachePushPlan {
  std::vector<std::string> paths;
  uint64_t bytes = 0;
  size_t served = 0;   // closure paths a pull cache serves, skipped
  size_t present = 0;  // closure paths the target already holds, skipped
};

// Plans the upload of `root`'s closure in `source`: everything except what a
// pull cache serves with a trusted signature and what `target` (when given)
// already holds.
auto plan_cache_push(nix::Store& source, const std::string& root,
                     nix::Store* target) -> CachePushPlan;

// Makes `root`'s closure valid in `store`. A path a pull cache serves is
// substituted from it, the rest is copied from `own` (the built-in cache, when
// given). A path that is nowhere stays missing, for Nix to build. Returns
// whether `root` itself came from `own`.
auto fetch_closure(nix::Store& store, const std::string& root, nix::Store* own,
                   const CacheTransferReport& report) -> bool;

// "1.2 GB": decimal units, one decimal below 10.
inline auto format_bytes(uint64_t bytes) -> std::string {
  static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1000 && unit + 1 < std::size(kUnits)) {
    value /= 1000;
    ++unit;
  }
  char text[32];
  if (unit == 0)
    std::snprintf(text, sizeof text, "%llu B",
                  static_cast<unsigned long long>(bytes));
  else
    std::snprintf(text, sizeof text, value < 10 ? "%.1f %s" : "%.0f %s", value,
                  kUnits[unit]);
  return text;
}

// "12/340 · 1.2 GB / 4.0 GB": a transfer's progress for activity details.
inline auto format_progress(const CacheTransferProgress& progress)
    -> std::string {
  return std::to_string(progress.done) + "/" + std::to_string(progress.total) +
         " \u00b7 " + format_bytes(progress.bytes_done) + " / " +
         format_bytes(progress.bytes_total);
}

}  // namespace tsugumi
