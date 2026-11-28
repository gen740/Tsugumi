#pragma once

#include <condition_variable>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "host/registry.hh"
#include "host/state_gate.hh"
#include "tsugumi/protocol/agent_generated.h"
#include "tsugumi/rpc/transport.hh"
#include "tsugumi/util/sqlite.hh"

namespace tsugumi::agent {
struct ConversationStore;
}
namespace tsugumi::host {
// The prefix of a decision's description, before the call's preview.
inline constexpr std::string_view kDecisionMark = "operation_approval\n";

struct Decision {
  std::string id;
  std::string preview;
};

// The one place a tool call waits for the person: saves that widen a Card's
// permissions or pass it a host secret, and the per-call acts on run data,
// versions, locks and Host templates. Each answer applies to one call with
// those exact arguments. Everything else the Agent does needs no answer.
struct Decisions {
  struct Dependencies {
    std::function<void(const std::string&, const std::string&)> event;
  };
  Decisions(StateGate& gate, sql::Db& db, ProjectRegistry& registry,
            agent::ConversationStore& conversations, bool& stopping,
            Dependencies dependencies);
  auto Find(const std::string& conversation, const std::string& call,
            const std::string& digest) -> std::optional<Decision>;
  auto Request(const std::string& project, const std::string& conversation,
               const std::string& call, const std::string& digest,
               const std::string& preview) -> Decision;
  // Returns once the person allowed it. Without wait (an external agent), a
  // pending decision fails with decision_required, naming what waits.
  auto Wait(std::unique_lock<std::recursive_mutex>& lock,
            const std::string& id, bool wait, std::stop_token stop) -> void;
  // The authenticated dispatch path holds the shared state gate.
  auto Handle(const RequestIn& request, bool human) -> std::string;
  // What waits, in one line, for a tool result and the logs.
  static auto Summary(const std::string& preview) -> std::string;

  std::recursive_mutex& mutex;
  sql::Db& db;
  ProjectRegistry& registry;
  agent::ConversationStore* conversations;
  bool& stopping;
  Dependencies dependencies;
  std::condition_variable_any changed;
};
}  // namespace tsugumi::host
