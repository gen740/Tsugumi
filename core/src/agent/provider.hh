// Model transport belongs to the core. It receives authorized context only;
// it has no project, filesystem, or RPC access of its own.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tsugumi::agent {

enum class ProviderAPI { AnthropicMessages, OpenAIResponses };
enum class ProviderAuth { None, ApiKey, ChatGPT };

struct ProviderConfig {
  ProviderAPI api = ProviderAPI::AnthropicMessages;
  ProviderAuth auth = ProviderAuth::ApiKey;
  std::string base_url = "https://api.anthropic.com";
  std::string model;
  std::string credential;
  // A credential refresh is owned by the core host, never by a UI transport.
  // The argument is the refused token, or empty for the initial request.
  std::function<std::string(std::string_view)> access_token;
  std::string effort;
  uint32_t max_tokens = 16384;
  uint32_t context_limit = 600000;
  uint32_t timeout_seconds = 600;
  bool web_search = false;
  bool fast = false;
};

struct Image {
  std::string media_type;
  std::string data;
};

struct WebSource {
  std::string url;
  std::string title;
};

struct ContentBlock {
  // text, tool_use, tool_result, web_search. Provider-only blocks stay in
  // Message::provider_content and are never interpreted as executable tools.
  std::string type = "text";
  std::string text;
  std::string id;
  std::string name;
  std::string input = "{}";
  bool is_error = false;
  std::vector<Image> images;
  // Provenance is retained through persistence and compaction. The core host
  // must authorize these resources before any subsequent model request.
  std::vector<std::string> resource_ids;
  std::vector<WebSource> sources;
  std::string error;
};

struct Message {
  std::string role = "user";
  std::vector<ContentBlock> content;
  std::string display;
  // Anthropic's complete content array, including signed thinking/search
  // blocks. OpenAI's complete output array when provider_api is Responses.
  std::string provider_content;
  ProviderAPI provider_api = ProviderAPI::AnthropicMessages;
  // Model context produced by compaction, never a user-authored message.
  bool context_summary = false;
};

struct ToolSpec {
  std::string name;
  std::string description;
  std::string input_schema = "{\"type\":\"object\",\"properties\":{}}";
};

struct Usage {
  uint64_t input_tokens = 0;
  uint64_t output_tokens = 0;
  uint64_t cache_read_tokens = 0;
  uint64_t cache_write_tokens = 0;
};

struct ModelEvent {
  // text, toolInput, searchStarted, searchFinished, citation.
  std::string kind;
  std::string text;
  std::string id;
  std::string name;
  uint32_t index = 0;
};

struct ModelReply {
  Message message{"assistant", {}, {}, {}, ProviderAPI::AnthropicMessages};
  std::string stop_reason;
  Usage usage;
};

class Cancelled final : public std::runtime_error {
 public:
  Cancelled() : std::runtime_error("cancelled") {}
};

class ProviderError final : public std::runtime_error {
 public:
  ProviderError(std::string message, bool retryable = false, int status = 0,
                std::chrono::milliseconds retry_after = {});
  bool retryable;
  int status;
  std::chrono::milliseconds retry_after;
};

// Runs one HTTP streaming request on the caller's worker thread. The callback
// is synchronous and may throw; no exception crosses a libcurl C callback.
auto StreamReply(const ProviderConfig& config, std::string_view system,
                 const std::vector<Message>& messages,
                 const std::vector<ToolSpec>& tools, std::stop_token stop,
                 const std::function<void(const ModelEvent&)>& event)
    -> ModelReply;

auto ListModels(const ProviderConfig& config, std::stop_token stop)
    -> std::vector<std::pair<std::string, std::string>>;

struct CredentialRefresh {
  std::string endpoint;
  std::string client_id;
  std::string refresh_token;
  std::string resource;
};

struct RefreshedCredential {
  std::string access_token;
  std::string refresh_token;
  std::string id_token;
  std::string scope;
  uint64_t expires_in = 3600;
};

// The host serializes refreshes per registration and persists a rotated
// refresh token together with its access token before using either one.
auto RefreshCredential(const CredentialRefresh& request, std::stop_token stop)
    -> RefreshedCredential;
auto RevokeCredential(std::string_view endpoint, std::string_view client_id,
                      std::string_view refresh_token, std::stop_token stop)
    -> void;

auto SerializeMessages(const std::vector<Message>& messages) -> std::string;
auto ParseMessages(std::string_view json) -> std::vector<Message>;
auto ValidToolArguments(std::string_view json) -> bool;

}  // namespace tsugumi::agent
