#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "worker_condition.hh"

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
  for (size_t i = 0; i < workers.size(); ++i) {
    const auto& worker = workers[i];
    if (service != "auto" && service != worker.id) continue;
    if (!resident.empty() && resident != worker.id) continue;
    if (resident.empty() && condition && !condition->matches(worker.grade)) continue;
    result.matches = true;
    if (!worker.free) continue;
    if (!result.index || worker.grade < workers[*result.index].grade) result.index = i;
  }
  return result;
}
}  // namespace tsugumi
