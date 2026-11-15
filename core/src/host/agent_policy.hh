#pragma once

#include <string_view>

#include "agent/json.hh"

namespace tsugumi::host {

// Only the host chooses the restrictions for model/plugin project calls.
inline auto RestrictAgentRequest(std::string_view name,
                                 agent::json::Members& arguments) -> void {
  if (name == "project_save" || name == "project_reload")
    arguments["check_permissions"] = "true";
  if (name == "project_shell") {
    arguments.erase("origin");
    for (const auto* field :
         {"read_only", "hide_credentials", "no_project_socket"})
      arguments[field] = "true";
  }
}

}  // namespace tsugumi::host
