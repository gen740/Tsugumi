#pragma once

#include <condition_variable>
#include <functional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "agent/runtime.hh"
#include "host/registry.hh"
#include "host/state_gate.hh"
#include "server.hh"
#include "tsugumi/protocol/agent_generated.h"
#include "util/sqlite.hh"

namespace tsugumi::agent {
struct ConversationStore;
}
namespace tsugumi::host {
struct Access {
  struct Dependencies {
    std::function<void(const std::string&, const std::string&)> event;
    std::function<void(const std::string&, const std::string&)> cancel;
  };
  Access(StateGate& gate, sql::Db& db, ProjectRegistry& registry,
         agent::ConversationStore& conversations, bool& stopping,
         Dependencies dependencies);
  auto HasGrant(const std::string& project, const std::string& conversation,
                Tsugumi::Agent::Access access) -> bool;
  auto AccessDescription(const std::string& resource,
                         Tsugumi::Agent::Access access,
                         Tsugumi::Agent::GrantScope scope,
                         const std::string& reason, bool needed = true)
      -> std::string;
  auto AccessDescriptions(const std::string& id, const std::string& project,
                          Tsugumi::Agent::Access needed,
                          Tsugumi::Agent::GrantScope scope,
                          const std::string& reason)
      -> std::vector<std::string>;
  auto RequestAccess(const std::string& project,
                     const std::string& conversation,
                     Tsugumi::Agent::Access access,
                     Tsugumi::Agent::GrantScope scope,
                     const std::string& reason, bool per_call = false,
                     bool combine = false) -> std::string;
  auto Require(const std::string& project, const std::string& conversation,
               Tsugumi::Agent::Access access, bool wait, std::stop_token stop,
               const std::string& reason, const std::string& authorization = {})
      -> void;
  auto AuthorizeContext(const std::string& id,
                        const std::vector<agent::Message>& messages) -> void;
  // The authenticated dispatch path holds the shared state gate.
  auto Handle(const RequestIn& request, bool human) -> std::string;

  std::recursive_mutex& mutex;
  sql::Db& db;
  ProjectRegistry& registry;
  agent::ConversationStore* conversations;
  bool& stopping;
  Dependencies dependencies;
  std::condition_variable_any changed;
};
}  // namespace tsugumi::host
