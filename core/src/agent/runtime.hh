// A model/tool loop, with all effects mediated by the owning core service.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "agent/context.hh"
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
  // compacting, compacted, error, finished, cancelled.
  std::string kind;
  std::string text;
  std::string id;
  std::string name;
  bool ok = true;
  uint32_t index = 0;
  Usage usage;
};

struct RuntimeHooks {
  // Durable service-owned sequence, including across transcript compaction.
  std::function<std::string()> allocate_call;
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
  uint32_t max_retries = 3;
  uint32_t max_cut_replies = 3;
  bool compact = true;
};

enum class RunOutcome { Finished, Cancelled, Failed };

struct RunResult {
  RunOutcome outcome = RunOutcome::Finished;
  Usage usage;
  std::string error;
};

// Ephemeral baseline for the last normal model request in one conversation.
// Restarts intentionally fall back to estimation, never cumulative UI usage.
struct ContextUsage {
  uint64_t input_tokens = 0;
  size_t message_count = 0;
  std::string prefix_hash;
  std::string request_hash;
  size_t omitted_before = 0;
  size_t result_limit = kToolResultBytes;
};

auto EstimateTextTokens(std::string_view text) -> uint64_t;
auto EstimateMessageTokens(const Message& message) -> uint64_t;

class Runtime {
 public:
  explicit Runtime(RuntimeOptions options = {}) : options_(options) {}
  auto Run(const ProviderConfig& config, std::string_view system,
           std::vector<Message>& messages, const std::vector<ToolSpec>& tools,
           std::stop_token stop, const RuntimeHooks& hooks,
           ContextUsage* context = nullptr) const -> RunResult;
  auto Compact(const ProviderConfig& config, std::string_view system,
               std::vector<Message>& messages,
               const std::vector<ToolSpec>& tools, std::stop_token stop,
               const RuntimeHooks& hooks, ContextUsage* context = nullptr) const
      -> void;

 private:
  RuntimeOptions options_;
};

}  // namespace tsugumi::agent
