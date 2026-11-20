// The Agent RPC operations Claude Code reaches as agent_* tools, described
// from the embedded agent.bfbs: one tool per request, its input the request
// table as JSON Schema.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tsugumi::cli {

struct AgentOperation {
  std::string tool;          // agent_conversation_list
  std::string request;       // ConversationListRequest (the union member)
  std::string description;
  std::string input_schema;  // JSON
};

auto AgentOperations() -> const std::vector<AgentOperation>&;
auto AgentSchema() -> std::pair<const uint8_t*, size_t>;

}  // namespace tsugumi::cli
