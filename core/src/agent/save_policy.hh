// Agent-only save policy. The project service transports metadata as bytes.
#pragma once
#include <functional>

#include "host/comments.hh"

namespace tsugumi::agent {
using ProjectRead =
    std::function<json::Members(const std::string&, const json::Members&)>;
struct SaveRevisionConflict : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct AgentSave {
  json::Members arguments;
  std::string comments;
};
auto PrepareAgentSave(json::Members arguments, const ProjectRead& read)
    -> AgentSave;
}  // namespace tsugumi::agent
