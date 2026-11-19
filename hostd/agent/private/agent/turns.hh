#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agent/provider.hh"
#include "agent/runtime.hh"
#include "agent/store.hh"
#include "agent/thought_schedule.hh"
#include "host/authorization.hh"
#include "host/state_gate.hh"
#include "tsugumi/rpc/transport.hh"

namespace tsugumi::host {
class Settings;
struct Access;
}
namespace tsugumi::agent {
struct Providers;
struct Turns {
  using Conversation = ConversationStore::Conversation;
  struct Dependencies {
    std::function<std::string(std::string)> scrub;
    std::function<void(const std::string&, const std::string&)> event;
    std::function<RuntimeHooks(const std::string&)> hooks;
    std::function<std::vector<ToolSpec>(bool)> specs;
    std::function<std::string(const std::string&, const std::string&, size_t,
                              const std::string&, std::stop_token,
                              const std::optional<host::TurnAuthorization>&)>
        run_helper;
    std::function<void(const std::string&)> cancel_file_tools;
    std::function<bool(const std::string&, const std::string&, std::stop_token)>
        excluded = {};
  };
  Turns(host::StateGate& gate, sql::Db& db, ConversationStore& conversations,
        Providers& providers, host::Settings& settings,
        std::stop_source& shutdown, bool& stopping,
        host::Access& access,
        Dependencies dependencies);
  static auto System(bool guide = false) -> std::string;
  auto ThoughtsFor(const std::string& id) -> std::shared_ptr<ThoughtSchedule>;
  auto ScrubSend(const std::string& frame) -> std::string;
  auto RecordOperation(const std::string& id, const std::string& kind,
                       const std::string& call = {},
                       const std::string& name = {},
                       const std::string& arguments = {}) -> void;
  auto ConversationResult(uint64_t request_id, const Conversation& c)
      -> std::string;
  auto Summarize(const ProviderConfig& config, const std::string& instruction,
                 std::vector<Message> messages) -> std::string;
  auto SkillContent(const std::string& id,
                    const std::vector<std::string>& names, bool automatic,
                    std::stop_token stop,
                    std::string* trusted_project = nullptr) -> ContentBlock;
  auto SkillCatalog(const Conversation& conversation)
      -> std::pair<std::string, Tsugumi::Host::SkillsListT>;
  auto AutomaticSkills(const std::string& id, std::stop_token stop,
                       RuntimeHooks& hooks) -> std::string;
  auto ScopedSkillTask(const Tsugumi::Agent::ConversationSendRequest* request,
                       const Conversation& conversation) -> std::string;
  auto PrepareSend(const Tsugumi::Agent::ConversationSendRequest* request,
                   std::stop_token stop) -> ContentBlock;
  auto Start(bool human, const std::string& id, const std::string& turn,
             const std::string& text,
             Tsugumi::Agent::Action action = Tsugumi::Agent::Action::Resume,
             std::string pending_frame = {}, const std::string& display = {})
      -> void;
  auto SummaryReply(uint64_t id, std::string_view raw) -> std::string;
  auto AddUsage(const std::string& conversation, const Usage& usage) -> void;
  auto Handle(const RequestIn& request, bool human,
              std::unique_lock<std::recursive_mutex>& lock) -> std::string;

  std::recursive_mutex& mutex;
  sql::Db& db;
  ConversationStore* conversations;
  Providers* providers;
  host::Settings* host_settings;
  std::stop_source& shutdown;
  bool& stopping;
  host::Access& access_service;
  Dependencies dependencies;
  std::map<std::string, std::shared_ptr<std::jthread>> jobs;
  std::set<std::string> running;
  std::map<std::string, std::shared_ptr<ContextUsage>> context_usage;
  std::map<std::string, std::shared_ptr<ThoughtSchedule>> thoughts;
};
}  // namespace tsugumi::agent
