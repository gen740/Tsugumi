#pragma once

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
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
#include "host/state_gate.hh"
#include "server.hh"

namespace tsugumi::host {
class Settings;
}
namespace tsugumi::host {
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
                              const std::string&)>
        run_helper;
    std::function<void(const std::string&)> cancel_shells;
  };
  Turns(host::StateGate& gate, sql::Db& db, ConversationStore& conversations,
        Providers& providers, host::Settings& settings,
        std::stop_source& shutdown, bool& stopping,
        std::condition_variable_any& changed, host::Access& access,
        Dependencies dependencies);
  static auto System(bool guide = false) -> std::string;
  static auto Excerpt(std::string_view text, size_t position = 0,
                      size_t limit = 240) -> std::string;
  auto ThoughtsFor(const std::string& id) -> std::shared_ptr<ThoughtSchedule>;
  auto ScrubSend(const std::string& frame) -> std::string;
  auto RecordOperation(const std::string& id, const std::string& kind,
                       const std::string& call = {},
                       const std::string& name = {},
                       const std::string& arguments = {}) -> void;
  auto ConversationResult(uint64_t request_id, const Conversation& c)
      -> std::string;
  auto Summarize(const std::string& id, const ProviderConfig& config,
                 const std::string& instruction, std::vector<Message> messages)
      -> std::string;
  auto PrepareSend(const Tsugumi::Agent::ConversationSendRequest* p,
                   std::stop_token stop, std::string& task,
                   std::string& authorization) -> ContentBlock;
  auto Start(const std::string& id, const std::string& turn,
             const std::string& text,
             Tsugumi::Agent::Action action = Tsugumi::Agent::Action::Resume,
             const std::vector<std::string>& resources = {},
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
  std::condition_variable_any& changed;
  host::Access& access_service;
  Dependencies dependencies;
  std::map<std::string, std::shared_ptr<std::jthread>> jobs;
  std::set<std::string> running;
  std::map<std::string, std::shared_ptr<ThoughtSchedule>> thoughts;
};
}  // namespace tsugumi::agent
