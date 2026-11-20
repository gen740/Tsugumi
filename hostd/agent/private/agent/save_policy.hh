// Agent-only save policy. The project service transports metadata as bytes.
#pragma once
#include <functional>

#include "host/comments.hh"
#include "host/save.hh"

namespace tsugumi::agent {
struct AgentSave {
  json::Members arguments;
  std::string comments;
};
auto PrepareAgentSave(json::Members arguments, const host::SaveBase& base,
                      const host::SaveRead& read,
                      const comments::ReserveComment& reserve = {},
                      const host::SourcePatches& patches = {}) -> AgentSave;
}  // namespace tsugumi::agent
