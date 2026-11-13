#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {

struct ProjectFailure : std::runtime_error {
  ProjectFailure(std::string c, std::string message)
      : std::runtime_error(std::move(message)), code(std::move(c)) {}
  std::string code;
};

// Called under the host's request mutex. The daemon is the only registry
// writer; its lifetime state-directory lock also excludes other daemons.
class Projects {
 public:
  Projects(std::filesystem::path state, std::filesystem::path demos)
      : registry_(std::move(state) / "recent-projects.json"),
        demos_(std::move(demos)) {}
  static auto Handles(Tsugumi::Host::RequestPayload type) -> bool;
  auto Handle(const Tsugumi::Host::Request &request, uint64_t id)
      -> std::string;

private:
  auto Apply(const Tsugumi::Host::Request &request) -> std::string;
  auto Catalog(const std::string& language) const -> std::vector<std::string>;
  auto Recents() const -> std::vector<std::string>;
  std::filesystem::path registry_;
  std::filesystem::path demos_;
};

}  // namespace tsugumi::host
