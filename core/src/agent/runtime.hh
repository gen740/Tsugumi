// A model/tool loop, with all effects mediated by the owning core service.
#pragma once

#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "agent/provider.hh"

namespace tsugumi::agent {

struct ToolCall {
  std::string id;
  std::string name;
  std::string arguments;
};

struct ToolResult {
  std::string text;
  bool is_error = false;
  std::vector<Image> images;
  std::vector<std::string> resource_ids;
};

struct RuntimeEvent {
  // text, toolInput, toolStarted, toolFinished, retrying, discardText,
  // compacting, compacted, error, finished, stepLimit, cancelled.
  std::string kind;
  std::string text;
  std::string id;
  std::string name;
  bool ok = true;
  uint32_t index = 0;
  Usage usage;
};

struct RuntimeHooks {
  std::function<ToolResult(const ToolCall&, std::stop_token)> execute_tool;
  std::function<void(const RuntimeEvent&)> event;
  // Called before a model request, including retries and compaction. Throws
  // when a resource's grant is absent or was revoked. Required by Run().
  std::function<void(const std::vector<Message>&)> authorize_context;
  // Persist complete messages before executing any tool, and after each
  // result. This callback must throw on a failed commit.
  std::function<void(const std::vector<Message>&)> checkpoint;
  // An interrupted tool is reconciled by stable call ID, never sent blindly.
  // Return a completed receipt; return nullopt if it was never dispatched.
  std::function<std::optional<ToolResult>(const ToolCall&)> reconcile_tool;
};

struct RuntimeOptions {
  uint32_t max_steps = 50;
  uint32_t max_retries = 3;
  uint32_t max_cut_replies = 3;
  bool compact = true;
};

enum class RunOutcome { Finished, Cancelled, StepLimit, Failed };

struct RunResult {
  RunOutcome outcome = RunOutcome::Finished;
  Usage usage;
  std::string error;
};

class Runtime {
 public:
  explicit Runtime(RuntimeOptions options = {}) : options_(options) {}
  auto Run(const ProviderConfig& config, std::string_view system,
           std::vector<Message>& messages, const std::vector<ToolSpec>& tools,
           std::stop_token stop, const RuntimeHooks& hooks) const -> RunResult;
  auto Compact(const ProviderConfig& config, std::vector<Message>& messages,
               std::stop_token stop, const RuntimeHooks& hooks) const -> void;

 private:
  RuntimeOptions options_;
};

}  // namespace tsugumi::agent
