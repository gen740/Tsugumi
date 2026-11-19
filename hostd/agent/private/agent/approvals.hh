#pragma once

#include <condition_variable>
#include <functional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "agent/runtime.hh"
#include "host/state_gate.hh"
#include "tsugumi/protocol/agent_generated.h"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/sqlite.hh"

namespace tsugumi::host {
struct Access;
}
namespace tsugumi::agent {
struct ConversationStore;
struct Tools;
struct Approvals {
  Approvals(host::StateGate& gate, sql::Db& db,
            ConversationStore& conversations, Tools& tools, bool& stopping,
            std::condition_variable_any& changed, host::Access& access);
  auto ApproveOperation(const std::string& project,
                        const std::string& conversation, const ToolCall& call,
                        const std::string& digest, const json::Members& args,
                        bool wait, std::stop_token stop,
                        const std::vector<std::string>& permissions = {})
      -> std::vector<std::string>;

  std::recursive_mutex& mutex;
  sql::Db& db;
  ConversationStore* conversations;
  Tools* tools;
  bool& stopping;
  std::condition_variable_any& changed;
  host::Access& access_service;
};
}  // namespace tsugumi::agent
