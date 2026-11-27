#pragma once

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

auto jobs_json(const std::filesystem::path& root, bool all) -> std::string;
auto remove_jobs(const std::filesystem::path& root, bool active,
                 const std::vector<std::string>& names)
    -> std::vector<JobRemoval>;
auto jobs_main(std::filesystem::path root, std::span<char*> args) -> int;

}  // namespace tsugumi::workerd
