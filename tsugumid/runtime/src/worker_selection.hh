#pragma once

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>

namespace tsugumi {
struct WorkerCandidate {
  std::string_view id;
  bool free;
  bool supported;
  std::optional<double> score;
};
struct WorkerSelection {
  bool matches = false;
  std::optional<size_t> index;
};

// A measured Card waits for the highest-ranked worker, including ties resolved
// in registration order. With no probe, preserve the established free-worker
// order and a resident Card's affinity.
inline auto select_worker(std::span<const WorkerCandidate> workers,
                          bool measured, std::string_view resident = {})
    -> WorkerSelection {
  const auto eligible = [&](const WorkerCandidate& worker) {
    return worker.supported &&
           (!measured || (worker.score && *worker.score > 0));
  };
  const bool keep_resident =
      !measured && !resident.empty() &&
      std::ranges::any_of(workers, [&](const auto& worker) {
        return worker.id == resident && eligible(worker);
      });
  WorkerSelection result;
  std::optional<size_t> best;
  for (size_t i = 0; i < workers.size(); ++i) {
    const auto& worker = workers[i];
    if (!eligible(worker) || (keep_resident && resident != worker.id)) continue;
    result.matches = true;
    if (!measured && !worker.free) continue;
    if (!best || (measured && *worker.score > *workers[*best].score)) best = i;
  }
  if (best && workers[*best].free) result.index = best;
  return result;
}
}  // namespace tsugumi
