#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agent/catalog.hh"
#include "agent/helper_scope.hh"
#include "agent/runtime.hh"
#include "agent/store.hh"
#include "host/agent_exclude.hh"
#include "host/layout_saving.hh"
#include "host/project_operations.hh"
#include "host/save.hh"
#include "host/state_gate.hh"
#include "tsugumi/rpc/transport.hh"

namespace tsugumi::host {
class Settings;
struct DemoReferences;
}  // namespace tsugumi::host
namespace tsugumi::host {
struct Decisions;
}
namespace tsugumi::agent {
struct Providers;
struct Turns;
// A run or run_status call waits at most this long for the run to end.
inline constexpr uint64_t kMaxRunWaitSeconds = 1800;
// A failed run's result names this many failed Cards with their log tails.
inline constexpr size_t kFailedCardsShown = 5;
inline constexpr uint32_t kFailedLogTailBytes = 2048;
struct Tools {
  using Conversation = ConversationStore::Conversation;
  using Project = host::ProjectRegistry::Project;
  struct Dependencies {
    std::filesystem::path host_templates = {};
    std::function<std::string(std::string)> scrub;
    std::function<void(const std::string&, const std::string&)> event;
    std::function<std::string(const std::string&, const std::string&, size_t,
                              const std::string&, std::stop_token, bool)>
        run_helper;
    std::function<Tsugumi::Host::ViewWindowsT(const std::string&)> view_windows;
    std::function<Tsugumi::Host::ViewActionResultT(
        const Tsugumi::Host::ViewActionRequestT&, std::stop_token)>
        view_action;
    // conversation, project, tool name, result envelope of a model turn's
    // run or run_status call.
    std::function<void(const std::string&, const std::string&,
                       const std::string&, const std::string&)>
        run_result = {};
  };
  Tools(host::StateGate& gate, sql::Db& db, ConversationStore& conversations,
        Providers& providers, host::Settings& settings,
        host::DemoReferences& demos, Turns& turns, std::stop_source& shutdown,
        host::Decisions& decisions, Dependencies dependencies);
  auto SaveProject(const std::string& project, json::Members args,
                   std::stop_token stop,
                   const std::function<void(const host::SaveBase&)>& check = {})
      -> std::string;
  std::recursive_mutex save_mutex;
  host::ProjectOperations operations;
  auto EditProjectTool(const std::string& conversation,
                       const std::string& project, const ToolCall& call,
                       const json::Members& args, const std::string& operation,
                       HelperScope* boundary, bool recovering,
                       std::stop_token stop) -> std::string;
  auto OperateProject(const Tsugumi::Host::ProjectOperateRequestT& operation,
                      const host::OperationContext& context,
                      std::stop_token stop) -> host::OperationReply;
  auto ListTemplates(const std::string& project, bool agent, std::stop_token stop)
      -> Tsugumi::Host::TemplateListT;
  auto AddHostTemplate(const Tsugumi::Host::TemplateAddT& request,
                       std::stop_token stop,
                       const host::TemplatePathCheck& check = {}) -> void;
  auto Specs(bool guide = false) -> std::vector<ToolSpec>;
  auto PrepareLayoutSave(const std::string& project, const std::string& args,
                         std::stop_token stop) -> std::string;
  auto InspectProject(const std::string& project, std::stop_token stop)
      -> json::Members;
  auto OutsideProjectFiles(const std::string& project, const std::string& path,
                           const std::string& result, std::stop_token stop)
      -> std::string;
  auto ExclusionsFor(const std::string& project, const host::SaveBase& base)
      -> host::AgentExclude;
  auto Exclusions(const std::string& project, std::stop_token stop = {})
      -> host::AgentExclude;
  auto UnfilteredProjectCall(const std::string& project,
                             const std::string& name,
                             const std::string& arguments, std::stop_token stop)
      -> std::string;
  auto ProjectCall(const std::string& project, const std::string& name,
                   const std::string& arguments, std::stop_token stop = {},
                   const host::SaveBase* inspected = nullptr) -> std::string;
  auto FileOperation(const std::string& conversation,
                     const std::string& project, const std::string& name,
                     json::Members& args, const std::string& operation,
                     HelperScope* boundary, std::stop_token stop,
                     std::vector<std::string>& resources) -> std::string;
  auto InputTypesQuery(const std::string& project, json::Members args,
                       std::stop_token stop = {}) -> Tsugumi::Host::InputTypesT;
  auto RawProjectCall(const std::string& project, const std::string& name,
                      const std::string& arguments, std::stop_token stop = {},
                      std::string* raw = nullptr) -> std::string;
  auto ApproveOperation(const std::string& project,
                        const std::string& conversation, const ToolCall& call,
                        const std::string& digest, bool wait,
                        std::stop_token stop,
                        const std::vector<std::string>& permissions) -> void;
  auto ToolResources(const std::string& conversation,
                     const HelperScope* boundary) -> std::vector<std::string>;
  auto HelperReader(const std::string& conversation, std::stop_token stop,
                    const std::string& revision = {},
                    const host::SaveBase* inspected = nullptr)
      -> HelperScope::Reader;
  auto ScopedResult(HelperScope& scope, const ToolCall& call, ToolResult result,
                    const HelperScope::Reader& read) -> ToolResult;
  /** The model's view of a call's result (tool_views.hh). */
  auto ForModel(const ToolCall& call, ToolResult result) -> ToolResult;
  /** A failed run's result with its failed Cards and their log tails. */
  auto WithFailedCards(const std::string& project, std::string result,
                       std::stop_token stop) -> std::string;
  auto ExecuteTool(const std::string& conversation,
                   const std::string& requested_project, const ToolCall& call,
                   bool wait, std::stop_token stop, bool helper = false,
                   HelperScope* boundary = nullptr) -> ToolResult;
  auto AllocateCall(const std::string& conversation,
                    const std::string& scope = {}) -> std::string;
  auto Hooks(const std::string& id, bool helper = false,
             std::shared_ptr<HelperScope> boundary = {}) -> RuntimeHooks;
  auto RequireHelperPermission(const HelperScope& boundary) -> void;
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
  host::Decisions& decisions;
  Dependencies dependencies;
  Catalog catalog;
  struct FileCall {
    std::string project;
    std::shared_ptr<std::stop_source> stop;
  };
  std::map<std::pair<std::string, std::string>, FileCall> file_calls;
};
}  // namespace tsugumi::agent
