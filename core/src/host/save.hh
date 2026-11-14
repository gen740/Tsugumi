// Agent-only save policy. The project service transports metadata as bytes.
#pragma once
#include "host/comments.hh"
#include <functional>

namespace tsugumi::host {
using ProjectRead = std::function<agent::json::Members(
    const std::string &, const agent::json::Members &)>;
struct SaveRevisionConflict : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct AgentSave {
  agent::json::Members arguments;
  std::string comments;
};
auto PrepareAgentSave(agent::json::Members arguments, const ProjectRead &read)
    -> AgentSave;
auto CardStem(const std::string &name) -> std::string;
} // namespace tsugumi::host
