#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent/provider.hh"
#include "host/registry.hh"
#include "host/state_gate.hh"
#include "tsugumi/protocol/agent_generated.h"
#include "util/fs.hh"
#include "util/sqlite.hh"

namespace tsugumi::agent {
// Owns project conversation databases and their lifetime locks. The Home
// records share HostStore's connection. Requests retain the caller's gate and
// turns/tools/helpers retain their original transactions on these databases.
struct ConversationStore {
  enum class SaveMode { Checkpoint, PrepareSend };
  struct Conversation {
    std::string id;
    std::string project;
    std::string role;
    std::string title;
    std::string messages;
    int64_t updated = 0;
  };

  using Scrub = std::function<std::string(std::string)>;
  using HasGrant = std::function<bool(const std::string&, const std::string&,
                                      Tsugumi::Service::Access)>;
  using IsRunning = std::function<bool(const std::string&)>;
  ConversationStore(host::StateGate& gate, sql::Db& home,
                    host::ProjectRegistry& registry, Scrub scrub,
                    HasGrant has_grant, IsRunning is_running);
  ~ConversationStore();
  auto OpenedPath(const std::string& project)
      -> std::optional<std::filesystem::path>;
  auto ProjectDatabase(const std::string& project, bool create = false)
      -> sql::Db*;
  auto Databases(const std::string& project = {}) -> std::vector<sql::Db*>;
  auto FindConversationDatabase(const std::string& id) -> sql::Db*;
  auto ConversationDatabase(const std::string& id) -> sql::Db&;
  auto GetConversation(const std::string& id) -> Conversation;
  auto OwnConversation(const std::string& id, bool human) -> void;
  auto Resources(const std::string& id) -> std::vector<std::string>;
  auto RememberResource(const std::string& id, const std::string& project)
      -> void;
  auto AuthorizeHistory(const std::string& id) -> void;
  auto SaveMessages(const std::string& id, const std::vector<Message>& messages,
                    SaveMode mode = SaveMode::Checkpoint) -> void;
  auto ConversationJson(const Conversation& conversation) -> std::string;
  auto Create(const Tsugumi::Agent::ConversationCreateRequest& request,
              bool human) -> Conversation;
  auto List(const Tsugumi::Agent::ConversationListRequest& request, bool human)
      -> Tsugumi::Agent::ConversationListT;
  auto Delete(const std::string& id) -> void;
  auto Search(const Tsugumi::Agent::ConversationSearchRequest& request,
              bool human) -> Tsugumi::Agent::SearchResultT;
  auto Events(const Tsugumi::Agent::EventsRequest& request, bool human)
      -> Tsugumi::Agent::EventsPageT;
  auto RecordEvent(const std::string& id, const std::string& raw) -> void;

  struct ProjectStore;
  host::StateGate& gate;
  sql::Db& db;
  host::ProjectRegistry& registry;
  Scrub scrub;
  HasGrant has_grant;
  IsRunning is_running;
  std::map<std::string, std::unique_ptr<ProjectStore>> project_stores;
};
}  // namespace tsugumi::agent
