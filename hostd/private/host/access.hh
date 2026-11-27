#pragma once

#include <condition_variable>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "agent/runtime.hh"
#include "host/authorization.hh"
#include "host/registry.hh"
#include "host/state_gate.hh"
#include "tsugumi/protocol/agent_generated.h"
#include "tsugumi/rpc/transport.hh"
#include "tsugumi/util/sqlite.hh"

namespace tsugumi::agent {
struct ConversationStore;
}
namespace tsugumi::host {
enum class ApprovalKind {
  Operation = static_cast<int>(Tsugumi::Service::Access::CardPermissions),
  Turn = static_cast<int>(Tsugumi::Service::Access::HelperScope),
};
struct Approval {
  std::string id;
  std::string preview;
};

struct Access {
  struct Dependencies {
    std::function<void(const std::string&, const std::string&)> event;
    std::function<void(const std::string&)> cancel;
  };
  Access(StateGate& gate, sql::Db& db, ProjectRegistry& registry,
         agent::ConversationStore& conversations, bool& stopping,
         Dependencies dependencies);
  auto HasGrant(const std::string& project, Tsugumi::Service::Access access)
      -> bool;
  // A grant the person gives by their own action, such as attaching a folder.
  auto Grant(const std::string& resource, Tsugumi::Service::Access access)
      -> void;
  auto AccessDescription(const std::string& resource,
                         Tsugumi::Service::Access access,
                         const std::string& reason, bool needed = true)
      -> std::string;
  auto AccessDescriptions(const std::string& project,
                          Tsugumi::Service::Access needed,
                          const std::string& reason, int offered)
      -> std::vector<std::string>;
  auto RequestAccess(const std::string& project,
                     const std::string& conversation,
                     Tsugumi::Service::Access access, const std::string& reason,
                     bool combine = false) -> std::string;
  auto Require(const std::string& project, const std::string& conversation,
               Tsugumi::Service::Access access, bool wait, std::stop_token stop,
               const std::string& reason,
               const std::optional<TurnAuthorization>& authorization = {})
      -> void;
  auto FindApproval(ApprovalKind kind, const std::string& conversation,
                    const std::string& call, const std::string& digest)
      -> std::optional<Approval>;
  auto RequestApproval(ApprovalKind kind, const std::string& project,
                       const std::string& conversation, const std::string& call,
                       const std::string& digest, const std::string& preview)
      -> Approval;
  auto WaitApproval(std::unique_lock<std::recursive_mutex>& lock,
                    const std::string& id, bool wait, std::stop_token stop)
      -> void;
  auto AuthorizeTurn(const std::string& project,
                     const std::string& conversation, const std::string& turn,
                     const std::string& digest, const std::string& preview,
                     std::stop_token stop) -> TurnAuthorization;
  auto PublishRequest(const std::string& conversation, const std::string& id,
                      const std::vector<std::string>& descriptions,
                      const std::string& call_id = {}) -> void;
  auto AuthorizeContext(
      const std::vector<agent::Message>& messages,
      const std::optional<TurnAuthorization>& authorization = {}) -> void;
  auto TurnPending(const std::string& conversation, const std::string& turn,
                   const std::string& digest) -> bool;
  auto CancelTurnApproval(const std::string& conversation,
                          const std::string& turn) -> void;
  auto HandleHost(const Tsugumi::Host::Request& request, uint64_t id)
      -> std::string;
  auto Revoke(const std::string& resource, Tsugumi::Service::Access access)
      -> void;
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
