#pragma once

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>

#include "tsugumi/document/worker_condition.hh"

namespace tsugumi {
struct WorkerCandidate {
  std::string_view id;
  uint64_t grade;
  bool free;
};
struct WorkerSelection {
  bool matches = false;
  std::optional<size_t> index;
};
// Input order is registration order. Unavailable/busy workers still count as
// registered matches; their absence is different from an impossible condition.
inline auto select_worker(std::span<const WorkerCandidate> workers,
                          const worker_condition::Condition* condition,
                          std::string_view service = "auto",
                          std::string_view resident = {}) -> WorkerSelection {
  WorkerSelection result;
  const bool keep_resident =
      !resident.empty() &&
      std::any_of(workers.begin(), workers.end(), [&](const auto& worker) {
        return worker.id == resident &&
               (service == "auto" || service == worker.id) &&
               (!condition || condition->matches(worker.grade));
      });
  for (size_t i = 0; i < workers.size(); ++i) {
    const auto& worker = workers[i];
    if (service != "auto" && service != worker.id) continue;
    if (keep_resident && resident != worker.id) continue;
    if (condition && !condition->matches(worker.grade)) continue;
    result.matches = true;
    if (!worker.free) continue;
    if (!result.index || worker.grade < workers[*result.index].grade)
      result.index = i;
  }
  return result;
}
}  // namespace tsugumi
