#pragma once

#include <string>

#include "agent/json.hh"
#include "agent/provider.hh"
#include "host/state_gate.hh"
#include "util/sqlite.hh"

namespace tsugumi::host {
class Settings;
}
namespace tsugumi::agent {
// Conversation selections contain no credentials. Resolve against current
// host Settings when a turn, helper or summary needs a provider.
struct Providers {
  Providers(host::StateGate& gate, sql::Db& db, host::Settings& settings);
  // Called under the shared state gate, including during turn acceptance.
  auto Configure(const std::string& id, const std::string& selection) -> void;
  auto Selection(const std::string& id) -> json::Members;
  auto ForConversation(const std::string& id) -> ProviderConfig;
  auto Profile(const std::string& conversation) -> std::string;

  host::StateGate& gate;
  sql::Db& db;
  host::Settings& settings;
};
}  // namespace tsugumi::agent
