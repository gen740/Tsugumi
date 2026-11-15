#pragma once

#include <functional>
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "agent/catalog.hh"
#include "agent/helper_scope.hh"
#include "agent/runtime.hh"
#include "agent/store.hh"
#include "host/state_gate.hh"
#include "server.hh"

namespace tsugumi::host {
class Settings;
struct DemoReferences;
}  // namespace tsugumi::host
namespace tsugumi::host {
struct Access;
}
namespace tsugumi::agent {
struct Providers;
struct Turns;
struct Tools {
  using Conversation = ConversationStore::Conversation;
  using Project = host::ProjectRegistry::Project;
  struct Dependencies {
    std::function<std::string(std::string)> scrub;
    std::function<void(const std::string&, const std::string&)> event;
    std::function<std::vector<std::string>(
        const std::string&, const std::string&, const ToolCall&,
        const std::string&, const json::Members&, bool, std::stop_token,
        const std::vector<std::string>&)>
        approve_operation;
    std::function<std::string(const std::string&, const std::string&, size_t,
                              const std::string&, std::stop_token,
                              const std::string&)>
        run_helper;
    std::function<std::string(const std::string&, const std::string&,
                              const std::string&, size_t)>
        helper_result;
  };
  Tools(host::StateGate& gate, sql::Db& db, ConversationStore& conversations,
        Providers& providers, host::Settings& settings,
        host::DemoReferences& demos, Turns& turns, std::stop_source& shutdown,
        host::Access& access, Dependencies dependencies);
  auto Specs(bool guide = false) -> std::vector<ToolSpec>;
  auto PrepareLayoutSave(const std::string& project, const std::string& args,
                         std::stop_token stop) -> std::string;
  auto ProjectCall(const std::string& project, const std::string& name,
                   const std::string& arguments, std::stop_token stop = {})
      -> std::string;
  auto AnnotateSave(const std::string& operation, const std::string& result)
      -> std::string;
  auto RawProjectCall(const std::string& project, const std::string& name,
                      const std::string& arguments, std::stop_token stop = {},
                      std::string* raw = nullptr) -> std::string;
  auto HelperReader(const std::string& conversation, std::stop_token stop,
                    const std::string& revision = {},
                    const std::string& authorization = {})
      -> HelperScope::Reader;
  auto ScopedResult(HelperScope& scope, const ToolCall& call, ToolResult result,
                    const HelperScope::Reader& read) -> ToolResult;
  auto ExecuteTool(const std::string& conversation,
                   const std::string& requested_project, const ToolCall& call,
                   bool wait, std::stop_token stop, bool helper = false,
                   HelperScope* boundary = nullptr) -> ToolResult;
  auto Hooks(const std::string& id, bool helper = false,
             std::shared_ptr<HelperScope> boundary = {}) -> RuntimeHooks;
  auto Handle(const RequestIn& request, bool human,
              std::unique_lock<std::recursive_mutex>& lock) -> std::string;

  std::recursive_mutex& mutex;
  sql::Db& db;
  ConversationStore* conversations;
  Providers* providers;
  host::Settings* host_settings;
  host::DemoReferences& demo_references;
  host::ProjectRegistry& registry;
  Turns* turns;
  std::stop_source& shutdown;
  host::Access& access_service;
  Dependencies dependencies;
  Catalog catalog;
  struct ShellCall {
    std::string project;
    std::shared_ptr<std::stop_source> stop;
  };
  std::map<std::pair<std::string, std::string>, ShellCall> shell_calls;
};
}  // namespace tsugumi::agent
