#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace tsugumi {
struct WorkerCandidate {
  std::string_view id;
  uint64_t capabilities;
  bool free;
  bool supported = true;
};
struct WorkerSelection {
  bool matches = false;
  std::optional<size_t> index;
};

constexpr auto matches_capabilities(uint64_t card, uint64_t worker) -> bool {
  return (card & ~worker) == 0;
}

// Input order breaks ties. Busy workers still count as registered matches.
inline auto select_worker(std::span<const WorkerCandidate> workers,
                          uint64_t capabilities, std::string_view resident = {})
    -> WorkerSelection {
  const auto matches = [&](const WorkerCandidate& worker) {
    return worker.supported &&
           matches_capabilities(capabilities, worker.capabilities);
  };
  const bool keep_resident =
      !resident.empty() &&
      std::ranges::any_of(workers, [&](const auto& worker) {
        return worker.id == resident && matches(worker);
      });
  WorkerSelection result;
  for (size_t i = 0; i < workers.size(); ++i) {
    const auto& worker = workers[i];
    if (!matches(worker) || (keep_resident && resident != worker.id)) continue;
    result.matches = true;
    if (!worker.free) continue;
    if (!result.index || std::popcount(worker.capabilities) <
                             std::popcount(workers[*result.index].capabilities))
      result.index = i;
  }
  return result;
}
}  // namespace tsugumi
