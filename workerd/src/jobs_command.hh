#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace tsugumi::workerd {

enum class JobRemovalStatus {
  Removed,
  AlreadyReleased,
  ActiveConfirmationRequired,
  Supervised,
  Failed,
};

struct JobRemoval {
  std::string name;
  JobRemovalStatus status;
  std::string detail;
};

// One retained job as `jobs list` shows it, without the submitting core.
struct JobRow {
  std::string project_scope, job_id, state, last_written, reason;
  uint64_t bytes = 0;
};

auto jobs_json(const std::filesystem::path& root, bool all) -> std::string;
auto job_rows(const std::filesystem::path& root) -> std::vector<JobRow>;
auto remove_jobs(const std::filesystem::path& root, bool active,
                 const std::vector<std::string>& names)
    -> std::vector<JobRemoval>;
auto jobs_main(std::filesystem::path root, std::span<char*> args) -> int;

}  // namespace tsugumi::workerd
