// Presentation policy belongs to hostd; clients only draw these typed rows.
#pragma once

#include "tsugumi/protocol/agent_generated.h"

namespace tsugumi::agent {
auto Presentation(const Tsugumi::Agent::ConversationT& conversation)
    -> std::vector<std::unique_ptr<Tsugumi::Agent::ChatRowT>>;
// A conversation whose turn has ended (stopped, or the host restarted) has
// nothing in progress: a call that never got its result was interrupted.
auto SettleStopped(std::vector<std::unique_ptr<Tsugumi::Agent::ChatRowT>>& rows)
    -> void;
auto AccessGroups(const std::vector<std::string>& asked)
    -> std::vector<std::unique_ptr<Tsugumi::Agent::AccessGroupT>>;
}  // namespace tsugumi::agent
