#include "agent/provider.hh"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/log.hh"

namespace tsugumi::agent {
namespace {
using json::Encode;
using json::Quote;

using Json = hosq::json::GenericDom<>;
using Object = std::map<std::string, std::string>;
constexpr size_t kMaxResponseBytes = 64u << 20;
constexpr size_t kMaxEventBytes = 16u << 20;

auto Parse(std::string_view text) -> Json { return schema::capture(text); }

auto FormEscape(std::string_view value) -> std::string {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

auto String(const Json& json, std::string_view pointer,
            std::string fallback = {}) -> std::string {
  if (!json[pointer].exists()) return fallback;
  schema::v::String value;
  if (json[pointer].decode_into(value))
    throw ProviderError("invalid string in model response");
  return value.value();
}

auto Number(const Json& json, std::string_view pointer, uint64_t fallback = 0)
    -> uint64_t {
  if (!json[pointer].exists()) return fallback;
  schema::v::Uint64 value;
  if (json[pointer].decode_into(value))
    throw ProviderError("invalid number in model response");
  return value.value();
}

auto Boolean(const Json& json, std::string_view pointer, bool fallback = false)
    -> bool {
  if (!json[pointer].exists()) return fallback;
  schema::v::Bool value;
  if (json[pointer].decode_into(value))
    throw ProviderError("invalid boolean in model response");
  return value.value();
}

auto Raw(const Json& json, std::string_view pointer,
         std::string fallback = "null") -> std::string {
  return json[pointer].exists() ? std::string(json[pointer].source())
                                : fallback;
}

// Error fields may be absent or null even when a useful message is present.
auto ErrorString(const Json& json, std::string_view pointer,
                 std::string fallback = {}) -> std::string {
  return Raw(json, pointer).starts_with('"') ? String(json, pointer) : fallback;
}

auto Items(const Json& json, std::string_view pointer = "")
    -> std::vector<Json> {
  if (!json[pointer].exists()) return {};
  schema::v::Array<schema::Json> array;
  if (json[pointer].decode_into(array))
    throw ProviderError("invalid array in model response");
  return array.value();
}

auto Members(const Json& json) -> Object {
  schema::v::Mapping<schema::v::String, schema::Json> object;
  if (json[""].decode_into(object))
    throw ProviderError("invalid object in model response");
  Object result;
  for (const auto& [key, value] : object.value())
    result.emplace(key, schema::text(value));
  return result;
}

auto Strings(const std::vector<std::string>& values) -> std::string {
  std::vector<std::string> strings;
  strings.reserve(values.size());
  for (const auto& value : values) strings.push_back(Quote(value));
  return json::Encode(strings);
}

// Diagnostic text is never a raw response. Remove request strings, including
// credentials and context that a provider might echo in an error message.
auto RequestStrings(const Json& value, std::vector<std::string>& strings,
                    bool content = false, bool payload = false) -> void {
  const auto raw = value[""].source();
  if (raw.starts_with('{')) {
    for (const auto& [key, child] : Members(value)) {
      // Protocol labels (role, type, model, tool name) are metadata, not
      // context. Redacting them would obscure unrelated diagnostic words.
      if (!payload &&
          (key == "role" || key == "type" || key == "model" || key == "name"))
        continue;
      RequestStrings(
          Parse(child), strings,
          content || key == "text" || key == "instructions" ||
              key == "system" || key == "content" || key == "input" ||
              key == "output" || key == "arguments" || key == "description",
          payload || key == "arguments" ||
              (key == "input" && Raw(value, "/type") == Quote("tool_use")));
    }
  } else if (raw.starts_with('[')) {
    for (const auto& child : Items(value))
      RequestStrings(child, strings, content, payload);
  } else if (content && raw.starts_with('"')) {
    const auto text = String(value, "");
    if (!text.empty()) strings.push_back(text);
    if (payload && (text.starts_with('{') || text.starts_with('['))) {
      try {
        RequestStrings(Parse(text), strings, true, true);
      } catch (...) {
        // Partial streamed arguments still have their complete string redacted.
      }
    }
  }
}

auto DiagnosticText(std::string text, std::vector<std::string> secrets)
    -> std::string {
  std::sort(secrets.begin(), secrets.end(),
            [](const auto& a, const auto& b) { return a.size() > b.size(); });
  for (const auto& secret : secrets) {
    if (secret.empty()) continue;
    size_t at = 0;
    while ((at = text.find(secret, at)) != std::string::npos) {
      text.replace(at, secret.size(), "[redacted]");
      at += 10;
    }
  }
  return text.substr(0, 400);
}

auto LogFailure(std::string_view operation, const ProviderConfig& config,
                const ProviderError& error, std::string_view summary = {})
    -> void {
  log::warn(
      std::string(operation) + " api=" +
      (config.api == ProviderAPI::OpenAIResponses ? "responses" : "messages") +
      " auth=" +
      (config.auth == ProviderAuth::ChatGPT  ? "chatgpt"
       : config.auth == ProviderAuth::ApiKey ? "api_key"
                                             : "none") +
      " model=" + Quote(config.model) +
      " http_status=" + std::to_string(error.status) +
      " curl_code=" + std::to_string(error.curl_code) +
      " error_code=" + Quote(error.code) + " message=" + Quote(error.what()) +
      (summary.empty() ? "" : " stream=" + std::string(summary)));
}

auto TextBlock(std::string_view text) -> std::string {
  return Encode(Object{{"type", Quote("text")}, {"text", Quote(text)}});
}

auto ImageBlock(const Image& image, bool responses) -> std::string {
  if (responses) {
    return Encode(Object{{"type", Quote("input_image")},
                         {"image_url", Quote("data:" + image.media_type +
                                             ";base64," + image.data)}});
  }
  return Encode(
      Object{{"type", Quote("image")},
             {"source", Encode(Object{{"type", Quote("base64")},
                                      {"media_type", Quote(image.media_type)},
                                      {"data", Quote(image.data)}})}});
}

auto SearchText(const ContentBlock& block) -> std::string {
  std::string text = "Web search: " + block.text;
  for (const auto& source : block.sources) {
    text += "\n" + (source.title.empty() ? source.url
                                         : source.title + ": " + source.url);
  }
  if (!block.error.empty()) text += "\nSearch error: " + block.error;
  return text;
}

// The domains the search tool may use: an allow list with blocked domains
// removed, or the block list alone. A request is not given the search tool
// when an allow list was set but nothing on it is left.
struct SearchDomains {
  bool offered = false;
  std::vector<std::string> allowed, blocked;
};

auto ResolveSearchDomains(const ProviderConfig& config) -> SearchDomains {
  SearchDomains result;
  if (!config.web_search || config.auth == ProviderAuth::None) return result;
  const auto& blocked = config.search_blocked_domains;
  for (const auto& domain : config.search_allowed_domains)
    if (std::find(blocked.begin(), blocked.end(), domain) == blocked.end())
      result.allowed.push_back(domain);
  if (!config.search_allowed_domains.empty()) {
    result.offered = !result.allowed.empty();
    return result;
  }
  result.blocked = blocked;
  result.offered = true;
  return result;
}

auto QuotedList(const std::vector<std::string>& values) -> std::string {
  std::vector<std::string> quoted;
  for (const auto& value : values) quoted.push_back(Quote(value));
  return json::Encode(quoted);
}

auto AnthropicBlock(const ContentBlock& block) -> std::string {
  if (block.type == "tool_use") {
    return Encode(
        {{"type", Quote("tool_use")},
         {"id", Quote(block.id)},
         {"name", Quote(block.name)},
         {"input", ValidToolArguments(block.input) ? block.input : "{}"}});
  }
  if (block.type == "tool_result") {
    std::vector<std::string> content{TextBlock(block.text)};
    for (const auto& image : block.images)
      content.push_back(ImageBlock(image, false));
    return Encode({{"type", Quote("tool_result")},
                   {"tool_use_id", Quote(block.id)},
                   {"content", block.images.empty() ? Quote(block.text)
                                                    : json::Encode(content)},
                   {"is_error", block.is_error ? "true" : "false"}});
  }
  return TextBlock(block.type == "web_search" ? SearchText(block) : block.text);
}

auto MessagesBody(const ProviderConfig& config, std::string_view system,
                  const std::vector<Message>& messages,
                  const std::vector<ToolSpec>& tools) -> std::string {
  std::vector<std::string> wired;
  for (const auto& message : messages) {
    std::vector<std::string> content;
    if (!message.provider_content.empty() &&
        message.provider_api == ProviderAPI::AnthropicMessages) {
      // Preserve signed thinking and server-side tool results verbatim.
      for (const auto& block : Items(Parse(message.provider_content)))
        content.push_back(schema::text(block));
    } else {
      for (const auto& block : message.content)
        content.push_back(AnthropicBlock(block));
    }
    if (content.empty()) content.push_back(TextBlock("[empty message]"));
    wired.push_back(Encode(Object{{"role", Quote(message.role)},
                                  {"content", json::Encode(content)}}));
  }
  // Automatic caching covers messages too. Together with the two stable
  // explicit breakpoints below this uses three of the API's four slots.
  // Default TTL is five minutes; matching looks back at most 20 blocks.
  Object body{
      {"cache_control", R"({"type":"ephemeral"})"},
      {"model", Quote(config.model)},
      {"max_tokens", std::to_string(config.max_tokens)},
      {"stream", "true"},
      {"messages", json::Encode(wired)},
      {"system", json::Encode(std::vector<std::string>{Encode(
                     Object{{"type", Quote("text")},
                            {"text", Quote(system)},
                            {"cache_control", "{\"type\":\"ephemeral\"}"}})})}};
  if (!config.effort.empty())
    body["output_config"] = Encode(Object{{"effort", Quote(config.effort)}});
  if (config.fast) body["speed"] = Quote("fast");
  std::vector<std::string> definitions;
  for (const auto& tool : tools) {
    if (!ValidToolArguments(tool.input_schema))
      throw std::runtime_error("invalid tool input schema");
    definitions.push_back(
        Encode(Object{{"name", Quote(tool.name)},
                      {"description", Quote(tool.description)},
                      {"input_schema", tool.input_schema},
                      {"eager_input_streaming", "true"}}));
  }
  if (const auto search = ResolveSearchDomains(config); search.offered) {
    Object definition{{"type", Quote("web_search_20250305")},
                      {"name", Quote("web_search")},
                      {"max_uses", "5"}};
    // The Messages API takes one of the two lists per request. With both,
    // a blocked subdomain of an allowed domain is not excluded; the settings
    // note says so.
    if (!search.allowed.empty())
      definition["allowed_domains"] = QuotedList(search.allowed);
    else if (!search.blocked.empty())
      definition["blocked_domains"] = QuotedList(search.blocked);
    definitions.push_back(Encode(definition));
  }
  if (!definitions.empty()) {
    auto last = Members(Parse(definitions.back()));
    last["cache_control"] = "{\"type\":\"ephemeral\"}";
    definitions.back() = Encode(last);
    body["tools"] = json::Encode(definitions);
  }
  return Encode(body);
}

auto ResponsesInput(const std::vector<Message>& messages, bool plan)
    -> std::string {
  std::vector<std::string> input;
  for (const auto& message : messages) {
    if (message.role == "assistant" && !message.provider_content.empty() &&
        message.provider_api == ProviderAPI::OpenAIResponses) {
      for (const auto& item : Items(Parse(message.provider_content)))
        input.push_back(schema::text(item));
      continue;
    }
    std::vector<std::string> text;
    auto flush = [&] {
      if (text.empty()) return;
      input.push_back(Encode(Object{{"role", Quote(message.role)},
                                    {"content", json::Encode(text)}}));
      text.clear();
    };
    for (const auto& block : message.content) {
      if (block.type == "tool_use") {
        flush();
        Object call{
            {"type", Quote("function_call")},
            {"call_id", Quote(block.id)},
            {"name", Quote(block.name)},
            {"arguments",
             Quote(ValidToolArguments(block.input) ? block.input : "{}")}};
        if (plan) call["namespace"] = Quote("tsugumi");
        input.push_back(Encode(call));
      } else if (block.type == "tool_result") {
        flush();
        std::vector<std::string> output{Encode(
            {{"type", Quote("input_text")}, {"text", Quote(block.text)}})};
        for (const auto& image : block.images)
          output.push_back(ImageBlock(image, true));
        input.push_back(Encode(
            Object{{"type", Quote("function_call_output")},
                   {"call_id", Quote(block.id)},
                   {"output", block.images.empty() ? Quote(block.text)
                                                   : json::Encode(output)}}));
      } else {
        text.push_back(Encode(
            {{"type", Quote(message.role == "assistant" ? "output_text"
                                                        : "input_text")},
             {"text", Quote(block.type == "web_search" ? SearchText(block)
                                                       : block.text)}}));
      }
    }
    flush();
  }
  return json::Encode(input);
}

auto ResponsesBody(const ProviderConfig& config, std::string_view system,
                   const std::vector<Message>& messages,
                   const std::vector<ToolSpec>& tools) -> std::string {
  const bool plan = config.auth == ProviderAuth::ChatGPT;
  Object body{{"model", Quote(config.model)},
              {"instructions", Quote(system)},
              {"input", ResponsesInput(messages, plan)},
              {"stream", "true"},
              {"store", "false"}};
  if (!plan) body["max_output_tokens"] = std::to_string(config.max_tokens);
  if (!config.effort.empty())
    body["reasoning"] = Encode(Object{{"effort", Quote(config.effort)}});
  if (config.fast) body["service_tier"] = Quote("fast");
  std::vector<std::string> functions;
  for (const auto& tool : tools) {
    if (!ValidToolArguments(tool.input_schema))
      throw std::runtime_error("invalid tool input schema");
    functions.push_back(Encode(Object{{"type", Quote("function")},
                                      {"name", Quote(tool.name)},
                                      {"description", Quote(tool.description)},
                                      {"parameters", tool.input_schema},
                                      {"strict", "false"}}));
  }
  std::vector<std::string> definitions;
  if (plan && !functions.empty()) {
    definitions.push_back(
        Encode(Object{{"type", Quote("namespace")},
                      {"name", Quote("tsugumi")},
                      {"description", Quote("Authorized Tsugumi operations.")},
                      {"tools", json::Encode(functions)}}));
  } else {
    definitions = std::move(functions);
  }
  // The Responses API filters by an allow list only, so a block list alone
  // leaves the search tool out rather than searching without it.
  if (const auto search = ResolveSearchDomains(config);
      search.offered && search.blocked.empty()) {
    Object definition{{"type", Quote("web_search")}};
    if (!search.allowed.empty())
      definition["filters"] =
          Encode(Object{{"allowed_domains", QuotedList(search.allowed)}});
    definitions.push_back(Encode(definition));
    if (!plan) body["include"] = "[\"web_search_call.action.sources\"]";
  }
  if (!definitions.empty()) body["tools"] = json::Encode(definitions);
  return Encode(body);
}

auto Endpoint(const ProviderConfig& config, std::string_view path)
    -> std::string {
  auto base = config.base_url;
  if (!(base.starts_with("https://") || base.starts_with("http://"))) {
    throw std::runtime_error("model endpoint must use HTTP or HTTPS");
  }
  if (base.find_first_of("\r\n?#") != std::string::npos)
    throw std::runtime_error("invalid model endpoint");
  while (base.ends_with('/')) base.pop_back();
  if (path.empty()) return base;
  // Accept API roots ending in /v1 as well as provider host roots.
  if (base.ends_with("/v1") && path.starts_with("v1/")) path.remove_prefix(3);
  return base + '/' + std::string(path);
}

class SSE {
 public:
  explicit SSE(std::function<void(std::string_view, std::string_view)> consume)
      : consume_(std::move(consume)) {}

  auto Feed(std::string_view bytes) -> void {
    pending_.append(bytes);
    if (pending_.size() > kMaxEventBytes)
      throw ProviderError("model stream event is too large");
    size_t consumed = 0;
    for (;;) {
      const auto end = pending_.find('\n', consumed);
      if (end == std::string::npos) break;
      auto line = std::string_view(pending_).substr(consumed, end - consumed);
      if (line.ends_with('\r')) line.remove_suffix(1);
      Line(line);
      consumed = end + 1;
    }
    pending_.erase(0, consumed);
  }

  auto Finish() -> void {
    if (!pending_.empty()) Line(pending_);
    Dispatch();
  }

 private:
  auto Line(std::string_view line) -> void {
    if (line.empty()) {
      Dispatch();
    } else if (line.starts_with("event:")) {
      line.remove_prefix(6);
      if (line.starts_with(' ')) line.remove_prefix(1);
      event_ = line;
    } else if (line.starts_with("data:")) {
      line.remove_prefix(5);
      if (line.starts_with(' ')) line.remove_prefix(1);
      if (!data_.empty()) data_ += '\n';
      data_ += line;
      if (data_.size() > kMaxEventBytes)
        throw ProviderError("model stream event is too large");
    }
  }
  auto Dispatch() -> void {
    if (!data_.empty() && data_ != "[DONE]") consume_(event_, data_);
    data_.clear();
    event_.clear();
  }
  std::function<void(std::string_view, std::string_view)> consume_;
  std::string pending_;
  std::string data_;
  std::string event_;
};

struct RequestStatus {
  int http = 0;
  int curl = 0;
};

struct Transfer {
  std::stop_token stop;
  std::function<void(std::string_view)> receive;
  std::exception_ptr error;
  std::string body;
  long status = 0;
  std::chrono::milliseconds retry_after{};
  size_t bytes = 0;

  static auto Write(char* data, size_t size, size_t count,
                    void* context) noexcept -> size_t {
    auto& self = *static_cast<Transfer*>(context);
    if (count > std::numeric_limits<size_t>::max() / std::max(size, size_t{1}))
      return 0;
    const size_t length = size * count;
    try {
      if (self.stop.stop_requested()) throw Cancelled();
      self.bytes += length;
      if (self.bytes > kMaxResponseBytes)
        throw ProviderError("model response is too large");
      if (self.status == 200 && self.receive)
        self.receive({data, length});
      else
        self.body.append(
            data, std::min(length, kMaxResponseBytes - self.body.size()));
      return length;
    } catch (...) {
      self.error = std::current_exception();
      return 0;
    }
  }

  static auto Header(char* data, size_t size, size_t count,
                     void* context) noexcept -> size_t {
    auto& self = *static_cast<Transfer*>(context);
    if (count > std::numeric_limits<size_t>::max() / std::max(size, size_t{1}))
      return 0;
    const size_t length = size * count;
    try {
      std::string line(data, length);
      if (line.starts_with("HTTP/")) {
        const auto space = line.find(' ');
        if (space != std::string::npos) {
          std::from_chars(line.data() + space + 1, line.data() + line.size(),
                          self.status);
        }
        self.body.clear();
      } else {
        std::transform(line.begin(), line.end(), line.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        const auto colon = line.find(':');
        if (colon != std::string::npos &&
            (line.substr(0, colon) == "retry-after" ||
             line.substr(0, colon) == "retry-after-ms")) {
          auto value = std::string_view(line).substr(colon + 1);
          while (value.starts_with(' ')) value.remove_prefix(1);
          uint64_t delay = 0;
          if (std::from_chars(value.data(), value.data() + value.size(), delay)
                  .ec == std::errc{}) {
            const uint64_t units =
                line.substr(0, colon) == "retry-after" ? 1000 : 1;
            // Leave room to add the server's interval to the monotonic clock.
            const auto max_delay =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::duration::max() / 2)
                    .count();
            self.retry_after = std::chrono::milliseconds(
                std::min(delay, static_cast<uint64_t>(max_delay) / units) *
                units);
          }
        }
      }
      return length;
    } catch (...) {
      self.error = std::current_exception();
      return 0;
    }
  }

  static auto Progress(void* context, curl_off_t, curl_off_t, curl_off_t,
                       curl_off_t) noexcept -> int {
    return static_cast<Transfer*>(context)->stop.stop_requested() ? 1 : 0;
  }
};

auto InitCurl() -> void {
  static std::once_flag once;
  std::call_once(once, [] {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      throw std::runtime_error("cannot initialize model HTTP transport");
  });
}

auto Request(const ProviderConfig& config, std::string_view path,
             const std::optional<std::string>& body, std::stop_token stop,
             const std::function<void(std::string_view)>& receive,
             std::string_view credential,
             std::string_view content_type = "application/json",
             bool allow_no_content = false, RequestStatus* status = nullptr)
    -> std::string {
  if (stop.stop_requested()) throw Cancelled();
  InitCurl();
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                           curl_easy_cleanup);
  if (!curl) throw std::runtime_error("cannot create model HTTP request");
  const auto endpoint = Endpoint(config, path);
  if (credential.find_first_of("\r\n") != std::string_view::npos)
    throw std::runtime_error("invalid model credential");
  if (config.auth != ProviderAuth::None && credential.empty())
    throw std::runtime_error("model credential is not configured");
  curl_slist* list = nullptr;
  auto add = [&](const std::string& value) {
    auto* next = curl_slist_append(list, value.c_str());
    if (!next) {
      curl_slist_free_all(list);
      list = nullptr;
      throw std::bad_alloc();
    }
    list = next;
  };
  add("content-type: " + std::string(content_type));
  add("accept: text/event-stream, application/json");
  if (config.api == ProviderAPI::AnthropicMessages)
    add("anthropic-version: 2023-06-01");
  if (config.auth != ProviderAuth::None) {
    add((config.api == ProviderAPI::AnthropicMessages &&
                 config.auth == ProviderAuth::ApiKey
             ? "x-api-key: "
             : "authorization: Bearer ") +
        std::string(credential));
  }
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
      list, curl_slist_free_all);
  Transfer transfer{stop, receive, {}, {}, 0, {}, 0};
  curl_easy_setopt(curl.get(), CURLOPT_URL, endpoint.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
  // libcurl still reports connection and transport failures.
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 0L);
  curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 60L);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &Transfer::Write);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
  curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, &Transfer::Header);
  curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &transfer);
  curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, &Transfer::Progress);
  curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer);
  if (body) {
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body->data());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(body->size()));
  }
  const auto code = curl_easy_perform(curl.get());
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &transfer.status);
  if (status)
    *status = {static_cast<int>(transfer.status), static_cast<int>(code)};
  if (transfer.error) {
    try {
      std::rethrow_exception(transfer.error);
    } catch (ProviderError& error) {
      error.status = static_cast<int>(transfer.status);
      error.curl_code = static_cast<int>(code);
      throw;
    }
  }
  if (stop.stop_requested()) throw Cancelled();
  if (code != CURLE_OK) {
    const bool transient =
        code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST ||
        code == CURLE_RECV_ERROR || code == CURLE_SEND_ERROR ||
        code == CURLE_PARTIAL_FILE || code == CURLE_OPERATION_TIMEDOUT ||
        code == CURLE_GOT_NOTHING || code == CURLE_HTTP2;
    throw ProviderError(
        std::string("model connection failed: ") + curl_easy_strerror(code),
        transient, static_cast<int>(transfer.status), {}, {},
        static_cast<int>(code));
  }
  if (transfer.status != 200 && !(allow_no_content && transfer.status == 204)) {
    std::string detail;
    std::string error_code;
    try {
      const auto error = Parse(transfer.body);
      error_code = Raw(error, "/error").starts_with('"')
                       ? ErrorString(error, "/error")
                       : ErrorString(error, "/error/code",
                                     ErrorString(error, "/error/type",
                                                 ErrorString(error, "/code")));
      detail =
          ErrorString(error, "/error/message",
                      ErrorString(error, "/error_description",
                                  ErrorString(error, "/description",
                                              ErrorString(error, "/detail"))));
    } catch (...) {
    }
    std::vector<std::string> secrets{std::string(credential)};
    if (content_type == "application/x-www-form-urlencoded")
      secrets.push_back(FormEscape(credential));
    if (body && content_type == "application/json")
      RequestStrings(Parse(*body), secrets);
    // Token requests pass their refresh token as the redaction credential,
    // while auth=None keeps it out of HTTP headers.
    detail = DiagnosticText(detail, secrets);
    error_code = DiagnosticText(error_code, secrets);
    if (detail.empty()) detail = "request refused";
    if (config.auth == ProviderAuth::ChatGPT &&
        error_code == "subscription_sharing_usage_limit_exceeded")
      throw ProviderError(
          "ChatGPT usage limit reached: review your plan or this app's limit "
          "in ChatGPT settings",
          false, static_cast<int>(transfer.status), transfer.retry_after,
          error_code);
    throw ProviderError(
        "model returned HTTP " + std::to_string(transfer.status) + ": " +
            (error_code.empty() ? "" : error_code + ": ") + detail,
        (transfer.status == 429 || transfer.status >= 500) &&
            error_code != "subscription_sharing_usage_limit_exceeded",
        static_cast<int>(transfer.status), transfer.retry_after, error_code);
  }
  return transfer.body;
}

auto WithCredential(const ProviderConfig& config,
                    const std::function<std::string(std::string_view)>& request)
    -> std::string {
  const auto get_token = [&](std::string_view stale) {
    try {
      return config.access_token(stale);
    } catch (const ProviderError& error) {
      LogFailure("Model credential failed", config, error);
      throw;
    }
  };
  const auto credential =
      config.auth == ProviderAuth::ChatGPT && config.access_token
          ? get_token({})
          : config.credential;
  try {
    return request(credential);
  } catch (const ProviderError& error) {
    if (error.status != 401 || config.auth != ProviderAuth::ChatGPT ||
        !config.access_token)
      throw;
    return request(get_token(credential));
  }
}

struct ModelStream {
  ProviderAPI api;
  const std::function<void(const ModelEvent&)>& event;
  ModelReply reply;
  struct Block {
    Object value;
    std::string kind, name;
    Object fragments;
    std::optional<std::vector<std::string>> citations;
    std::string arguments;
    bool has_arguments = false;
    bool closed = false;

    auto Encoded() const -> std::string {
      auto fields = value;
      for (const auto& [key, text] : fragments) fields[key] = Quote(text);
      if (citations) fields["citations"] = json::Encode(*citations);
      return Encode(fields);
    }
  };
  std::map<uint32_t, Block> blocks;
  std::map<uint32_t, std::string> output;
  std::map<uint32_t, ContentBlock> calls;
  std::string text;
  bool finished = false;
  const std::function<std::string()>& allocate_call;
  std::vector<std::pair<std::string, size_t>> event_types;
  std::vector<std::string> output_items;
  bool terminal_output = false;
  size_t terminal_output_length = 0;
  bool remote_error = false;

  auto RecordItem(uint32_t index, const Json& item, std::string_view stage)
      -> void {
    Object metadata{{"stage", Quote(stage)}, {"index", std::to_string(index)}};
    for (const auto* field : {"type", "name", "namespace", "call_id"}) {
      const auto value = Raw(item, "/" + std::string(field));
      metadata[field] = value.starts_with('"') ? value : "null";
    }
    output_items.push_back(Encode(metadata));
  }

  auto Summary() const -> std::string {
    std::vector<std::string> events;
    for (const auto& [type, count] : event_types)
      events.push_back(Encode(
          Object{{"type", Quote(type)}, {"count", std::to_string(count)}}));
    size_t completed_call_count = 0;
    for (const auto& [index, item] : output) {
      (void)index;
      if (Raw(Parse(item), "/type") == Quote("function_call"))
        ++completed_call_count;
    }
    return Encode(
        {{"events", json::Encode(events)},
         {"items", json::Encode(output_items)},
         {"streamed_calls", std::to_string(calls.size())},
         {"completed_calls", std::to_string(completed_call_count)},
         {"terminal_output", terminal_output ? "true" : "false"},
         {"terminal_output_length", std::to_string(terminal_output_length)}});
  }

  auto Emit(ModelEvent value) const -> void {
    if (event) event(value);
  }

  auto AppendField(Block& block, std::string_view key, std::string_view delta)
      -> void {
    const auto [field, added] = block.fragments.try_emplace(std::string(key));
    if (added)
      if (const auto initial = block.value.find(field->first);
          initial != block.value.end())
        field->second = String(Parse(initial->second), "");
    field->second += delta;
  }

  auto Consume(std::string_view event_name, std::string_view data) -> void {
    const auto json = Parse(data);
    const auto type = String(json, "/type", std::string(event_name));
    if (api == ProviderAPI::OpenAIResponses) {
      if (!event_types.empty() && event_types.back().first == type)
        ++event_types.back().second;
      else
        event_types.emplace_back(type, 1);
    }
    if (finished) return;
    if (api == ProviderAPI::AnthropicMessages)
      Anthropic(json, type);
    else
      Responses(json, type);
  }

  auto Anthropic(const Json& json, const std::string& type) -> void {
    const auto n = Number(json, "/index");
    if (n > 4096) throw ProviderError("too many model content blocks");
    const auto index = static_cast<uint32_t>(n);
    if (type == "message_start") {
      reply.usage.cache_read_tokens =
          Number(json, "/message/usage/cache_read_input_tokens");
      reply.usage.cache_write_tokens =
          Number(json, "/message/usage/cache_creation_input_tokens");
      reply.usage.input_tokens = Number(json, "/message/usage/input_tokens") +
                                 reply.usage.cache_read_tokens +
                                 reply.usage.cache_write_tokens;
    } else if (type == "content_block_start") {
      if (blocks.contains(index))
        throw ProviderError("duplicate model content block");
      auto& block = blocks[index];
      block.value = Members(Parse(Raw(json, "/content_block")));
      block.kind = String(json, "/content_block/type");
      block.name = String(json, "/content_block/name");
      if (block.kind == "text") {
        const auto initial = String(json, "/content_block/text");
        if (!initial.empty()) Emit({"text", initial, {}, {}, index});
      }
    } else if (type == "content_block_delta") {
      auto it = blocks.find(index);
      if (it == blocks.end() || it->second.closed)
        throw ProviderError("model delta has no open content block");
      auto& block = it->second;
      const auto kind = String(json, "/delta/type");
      if (kind == "text_delta") {
        const auto delta = String(json, "/delta/text");
        AppendField(block, "text", delta);
        Emit({"text", delta, {}, {}, index});
      } else if (kind == "input_json_delta") {
        const auto delta = String(json, "/delta/partial_json");
        block.arguments += delta;
        block.has_arguments = true;
        if (block.kind == "tool_use")
          Emit({"toolInput", delta, {}, block.name, index});
      } else if (kind == "thinking_delta") {
        AppendField(block, "thinking", String(json, "/delta/thinking"));
      } else if (kind == "signature_delta") {
        AppendField(block, "signature", String(json, "/delta/signature"));
      } else if (kind == "citations_delta") {
        if (!block.citations) {
          block.citations.emplace();
          if (const auto initial = block.value.find("citations");
              initial != block.value.end())
            for (const auto& item : Items(Parse(initial->second)))
              block.citations->push_back(schema::text(item));
        }
        block.citations->push_back(Raw(json, "/delta/citation", "{}"));
        Emit({"citation",
              String(json, "/delta/citation/url"),
              {},
              String(json, "/delta/citation/title"),
              index});
      }
    } else if (type == "content_block_stop") {
      const auto it = blocks.find(index);
      if (it == blocks.end() || it->second.closed)
        throw ProviderError("model closed an unknown content block");
      auto& block = it->second;
      block.closed = true;
      if (block.has_arguments)
        block.value["input"] =
            ValidToolArguments(block.arguments) ? block.arguments : "{}";
      if (block.kind == "server_tool_use") {
        const auto value = Parse(block.Encoded());
        Emit({"searchStarted", String(value, "/input/query"),
              String(value, "/id"), "web_search", index});
      } else if (block.kind == "web_search_tool_result") {
        const auto value = Parse(block.Encoded());
        Emit({"searchFinished", Raw(value, "/content"),
              String(value, "/tool_use_id"), "web_search", index});
      }
    } else if (type == "message_delta") {
      reply.stop_reason = String(json, "/delta/stop_reason", reply.stop_reason);
      reply.usage.output_tokens =
          Number(json, "/usage/output_tokens", reply.usage.output_tokens);
    } else if (type == "message_stop") {
      finished = true;
    } else if (type == "error") {
      const auto code = ErrorString(json, "/error/type");
      remote_error = true;
      throw ProviderError(
          ErrorString(json, "/error/message", "model stream failed"),
          code == "overloaded_error" || code == "api_error" ||
              code == "rate_limit_error",
          0, {}, code);
    }
  }

  auto Responses(const Json& json, const std::string& type) -> void {
    const auto n = Number(json, "/output_index");
    if (n > 4096) throw ProviderError("too many model output items");
    const auto index = static_cast<uint32_t>(n);
    if (type == "response.output_text.delta") {
      const auto delta = String(json, "/delta");
      text += delta;
      Emit({"text", delta, {}, {}, index});
    } else if (type == "response.output_item.added") {
      RecordItem(index, Parse(Raw(json, "/item", "{}")), "added");
      if (String(json, "/item/type") == "function_call") {
        if (calls.contains(index))
          throw ProviderError("duplicate model tool call");
        auto& call = calls[index];
        call.type = "tool_use";
        call.id = String(json, "/item/call_id");
        call.name = String(json, "/item/name");
        if (call.name.starts_with("tsugumi.")) call.name.erase(0, 8);
        call.input = String(json, "/item/arguments");
      }
    } else if (type == "response.function_call_arguments.delta") {
      auto it = calls.find(index);
      if (it == calls.end())
        throw ProviderError("model arguments have no tool call");
      const auto delta = String(json, "/delta");
      it->second.input += delta;
      Emit({"toolInput", delta, {}, it->second.name, index});
    } else if (type == "response.function_call_arguments.done") {
      auto it = calls.find(index);
      if (it == calls.end())
        throw ProviderError("model arguments have no tool call");
      const auto complete = String(json, "/arguments");
      if (!it->second.input.empty() && it->second.input != complete)
        throw ProviderError("model arguments changed after streaming");
      it->second.input = complete;
    } else if (type == "response.output_item.done") {
      RecordItem(index, Parse(Raw(json, "/item", "{}")), "done");
      output[index] = Raw(json, "/item", "{}");
      if (String(json, "/item/type") == "web_search_call") {
        Emit({"searchStarted", String(json, "/item/action/query"),
              String(json, "/item/id"), "web_search", index});
        Emit({"searchFinished", Raw(json, "/item/action/sources", "[]"),
              String(json, "/item/id"), "web_search", index});
      }
    } else if (type == "response.completed" || type == "response.incomplete") {
      reply.usage.input_tokens = Number(json, "/response/usage/input_tokens");
      reply.usage.output_tokens = Number(json, "/response/usage/output_tokens");
      reply.usage.cache_read_tokens =
          Number(json, "/response/usage/input_tokens_details/cached_tokens");
      reply.stop_reason = type == "response.incomplete" ? "max_tokens"
                          : calls.empty()               ? "end_turn"
                                                        : "tool_use";
      // A non-empty terminal output is authoritative, including encrypted
      // reasoning. ChatGPT ends its streams with an empty one: then the
      // streamed items are the response.
      const auto terminal_items = Items(json, "/response/output");
      terminal_output = !terminal_items.empty();
      if (terminal_output) {
        output.clear();
        uint32_t i = 0;
        for (const auto& item : terminal_items) {
          RecordItem(i, item, "terminal");
          output[i++] = schema::text(item);
        }
        terminal_output_length = i;
      }
      finished = true;
    } else if (type == "response.failed" || type == "error") {
      terminal_output = json["/response/output"].exists();
      if (terminal_output) {
        output.clear();
        for (const auto& item : Items(json, "/response/output")) {
          RecordItem(static_cast<uint32_t>(output.size()), item, "terminal");
          output[static_cast<uint32_t>(output.size())] = schema::text(item);
        }
        terminal_output_length = output.size();
      }
      const auto code = ErrorString(
          json, "/response/error/code",
          ErrorString(json, "/error/code", ErrorString(json, "/code")));
      if (code == "subscription_sharing_usage_limit_exceeded")
        throw ProviderError(
            "ChatGPT usage limit reached: review your plan or this app's limit "
            "in ChatGPT settings",
            false, 0, {}, code);
      remote_error = true;
      throw ProviderError(
          ErrorString(json, "/response/error/message",
                      ErrorString(json, "/error/message",
                                  ErrorString(json, "/message",
                                              "model stream failed"))),
          code == "server_error" || code == "rate_limit_exceeded", 0, {}, code);
    }
  }

  auto Finish() -> ModelReply {
    if (!finished)
      throw ProviderError(
          "the model connection ended before its terminal event", true);
    reply.message.provider_api = api;
    std::vector<std::string> original;
    if (api == ProviderAPI::AnthropicMessages) {
      std::map<std::string, size_t> searches;
      for (const auto& [index, block] : blocks) {
        (void)index;
        if (!block.closed)
          throw ProviderError("the model response contains an unfinished block",
                              true);
        const auto value = Parse(block.Encoded());
        const auto type = String(value, "/type");
        original.push_back(schema::text(value));
        ContentBlock content;
        if (type == "text") {
          content.text = String(value, "/text");
        } else if (type == "tool_use") {
          content.type = "tool_use";
          content.id = String(value, "/id");
          content.name = String(value, "/name");
          content.input = block.has_arguments ? block.arguments
                                              : Raw(value, "/input", "{}");
        } else if (type == "server_tool_use") {
          content.type = "web_search";
          content.id = String(value, "/id");
          content.name = String(value, "/name");
          content.text = String(value, "/input/query");
          searches[content.id] = reply.message.content.size();
        } else if (type == "web_search_tool_result") {
          const auto found = searches.find(String(value, "/tool_use_id"));
          if (found != searches.end()) {
            auto& search = reply.message.content[found->second];
            const auto result = Parse(Raw(value, "/content", "[]"));
            if (result["/error_code"].exists())
              search.error = String(result, "/error_code");
            else {
              for (const auto& source : Items(result)) {
                const auto url = String(source, "/url");
                if (!url.empty())
                  search.sources.push_back({url, String(source, "/title")});
              }
            }
          }
          continue;
        } else {
          continue;
        }
        reply.message.content.push_back(std::move(content));
      }
      if (reply.stop_reason.empty())
        throw ProviderError("model response has no stop reason", true);
    } else {
      std::vector<ContentBlock> completed_calls;
      std::string completed_text;
      for (const auto& [index, item] : output) {
        (void)index;
        original.push_back(item);
        const auto value = Parse(item);
        const auto type = String(value, "/type");
        if (type == "function_call") {
          ContentBlock call;
          call.type = "tool_use";
          call.id = String(value, "/call_id");
          call.name = String(value, "/name");
          if (call.name.starts_with("tsugumi.")) call.name.erase(0, 8);
          call.input = String(value, "/arguments");
          const auto streamed = std::find_if(
              calls.begin(), calls.end(),
              [&](const auto& entry) { return entry.second.id == call.id; });
          if (streamed != calls.end() &&
              (streamed->second.name != call.name ||
               (!streamed->second.input.empty() &&
                streamed->second.input != call.input))) {
            throw ProviderError("model changed a completed tool call");
          }
          completed_calls.push_back(std::move(call));
        } else if (type == "message") {
          for (const auto& content : Items(value, "/content")) {
            if (String(content, "/type") == "output_text")
              completed_text += String(content, "/text");
            for (const auto& annotation : Items(content, "/annotations")) {
              if (String(annotation, "/type") == "url_citation") {
                Emit({"citation",
                      String(annotation, "/url"),
                      {},
                      String(annotation, "/title"),
                      index});
              }
            }
          }
        } else if (type == "web_search_call") {
          ContentBlock search;
          search.type = "web_search";
          search.id = String(value, "/id");
          search.text = String(value, "/action/query");
          if (String(value, "/status") == "failed") search.error = "failed";
          for (const auto& source : Items(value, "/action/sources")) {
            const auto url = String(source, "/url");
            if (!url.empty())
              search.sources.push_back({url, String(source, "/title")});
          }
          reply.message.content.push_back(std::move(search));
        }
      }
      if (completed_calls.size() != calls.size() && !calls.empty()) {
        throw ProviderError("model response contains an unfinished tool call",
                            true);
      }
      if (text.empty() && !completed_text.empty()) {
        text = completed_text;
        Emit({"text", text, {}, {}, 0});
      }
      if (!text.empty()) {
        ContentBlock content;
        content.text = text;
        reply.message.content.insert(reply.message.content.begin(),
                                     std::move(content));
      }
      for (auto& call : completed_calls)
        reply.message.content.push_back(std::move(call));
      if (!completed_calls.empty() && reply.stop_reason == "end_turn")
        reply.stop_reason = "tool_use";
    }
    if (!original.empty())
      reply.message.provider_content = json::Encode(original);
    std::set<std::string> ids;
    for (const auto& block : reply.message.content) {
      if (block.type == "tool_use" && (block.id.empty() || block.name.empty() ||
                                       !ids.insert(block.id).second)) {
        throw ProviderError("model returned an invalid or duplicate tool call");
      }
    }
    if (allocate_call && !reply.message.provider_content.empty()) {
      auto original_blocks = Items(Parse(reply.message.provider_content));
      std::vector<std::string> encoded;
      size_t next = 0;
      for (auto& block : reply.message.content) {
        if (block.type != "tool_use") continue;
        block.id = allocate_call();
        while (next < original_blocks.size()) {
          auto item = Members(original_blocks[next]);
          const auto kind = String(original_blocks[next], "/type");
          if (kind == "tool_use" || kind == "function_call") {
            item[kind == "tool_use" ? "id" : "call_id"] = Quote(block.id);
            if (kind == "function_call") item.erase("id");
            original_blocks[next++] = Parse(Encode(item));
            break;
          }
          ++next;
        }
      }
      for (const auto& item : original_blocks)
        encoded.push_back(schema::text(item));
      reply.message.provider_content = json::Encode(encoded);
    }
    return std::move(reply);
  }
};

}  // namespace

ProviderError::ProviderError(std::string message, bool retryable_value,
                             int status_value,
                             std::chrono::milliseconds retry_after_value,
                             std::string code_value, int curl_code_value)
    : std::runtime_error(std::move(message)),
      retryable(retryable_value),
      status(status_value),
      retry_after(retry_after_value),
      code(std::move(code_value)),
      curl_code(curl_code_value) {}

auto ProviderError::reason_code() const -> std::string {
  return sign_in_required ? "chatgpt_sign_in_required" : code;
}

auto ValidToolArguments(std::string_view json) -> bool {
  try {
    (void)Members(Parse(json));
    return true;
  } catch (...) {
    return false;
  }
}

auto StreamReply(const ProviderConfig& config, std::string_view system,
                 const std::vector<Message>& messages,
                 const std::vector<ToolSpec>& tools, std::stop_token stop,
                 const std::function<void(const ModelEvent&)>& event,
                 const std::function<std::string()>& allocate_call)
    -> ModelReply {
  if (config.model.empty()) throw std::runtime_error("model is not configured");
  const bool anthropic = config.api == ProviderAPI::AnthropicMessages;
  const auto body = anthropic ? MessagesBody(config, system, messages, tools)
                              : ResponsesBody(config, system, messages, tools);
  std::optional<ModelReply> reply;
  WithCredential(config, [&](std::string_view credential) {
    ModelStream stream{config.api, event,         {}, {}, {},    {}, {},
                       false,      allocate_call, {}, {}, false, 0,  false};
    SSE sse([&](std::string_view name, std::string_view data) {
      stream.Consume(name, data);
    });
    RequestStatus status;
    try {
      Request(
          config, anthropic ? "v1/messages" : "v1/responses", body, stop,
          [&](std::string_view data) { sse.Feed(data); }, credential,
          "application/json", false, &status);
      sse.Finish();
      reply = stream.Finish();
    } catch (const ProviderError& error) {
      std::string detail = error.what();
      std::string code = error.code;
      if (stream.remote_error) {
        std::vector<std::string> secrets{std::string(credential)};
        RequestStrings(Parse(body), secrets);
        if (!stream.text.empty()) secrets.push_back(stream.text);
        for (const auto& [index, call] : stream.calls) {
          (void)index;
          if (!call.input.empty()) secrets.push_back(call.input);
        }
        for (const auto& [index, item] : stream.output) {
          (void)index;
          RequestStrings(Parse(item), secrets);
        }
        for (const auto& [index, block] : stream.blocks) {
          (void)index;
          RequestStrings(Parse(block.Encoded()), secrets);
          if (!block.arguments.empty()) secrets.push_back(block.arguments);
        }
        detail = DiagnosticText(std::move(detail), secrets);
        code = DiagnosticText(std::move(code), secrets);
      }
      ProviderError safe(std::move(detail), error.retryable, status.http,
                         error.retry_after, std::move(code), status.curl);
      LogFailure("Model call failed", config, safe,
                 anthropic ? "" : stream.Summary());
      throw safe;
    }
    return std::string{};
  });
  return std::move(*reply);
}

auto ListModels(const ProviderConfig& config, std::stop_token stop)
    -> std::vector<ModelInfo> {
  const auto body = WithCredential(config, [&](std::string_view credential) {
    try {
      return Request(config, "v1/models", std::nullopt, stop, {}, credential);
    } catch (const ProviderError& error) {
      LogFailure("Model catalog failed", config, error);
      throw;
    }
  });
  std::vector<ModelInfo> models;
  const bool plan = config.auth == ProviderAuth::ChatGPT;
  const auto catalog = Parse(body);
  for (const auto& item : Items(catalog, plan ? "/models" : "/data")) {
    if (plan && String(item, "/visibility") != "list") continue;
    const auto id = String(item, plan ? "/slug" : "/id");
    // Anthropic reports max_input_tokens; the ChatGPT account catalog reports
    // its active context_window (or max_context_window when no active window
    // is specified). The public OpenAI API-key catalog reports neither.
    const auto nullable_number = [&](std::string_view field) {
      return Raw(item, field) == "null" ? uint64_t{0} : Number(item, field);
    };
    auto window =
        nullable_number(plan ? "/context_window" : "/max_input_tokens");
    if (plan && !window) window = nullable_number("/max_context_window");
    if (!id.empty())
      models.push_back({id, String(item, "/display_name", id),
                        static_cast<uint32_t>(std::min<uint64_t>(
                            window, std::numeric_limits<uint32_t>::max()))});
  }
  return models;
}

auto RefreshCredential(const CredentialRefresh& request, std::stop_token stop)
    -> RefreshedCredential {
  if (request.endpoint.empty() || request.client_id.empty() ||
      request.refresh_token.empty() || request.resource.empty()) {
    throw std::runtime_error("ChatGPT token refresh is not configured");
  }
  const auto body =
      "grant_type=refresh_token&client_id=" + FormEscape(request.client_id) +
      "&refresh_token=" + FormEscape(request.refresh_token) +
      "&resource=" + FormEscape(request.resource);
  ProviderConfig config;
  config.api = ProviderAPI::OpenAIResponses;
  config.auth = ProviderAuth::None;
  config.base_url = request.endpoint;
  RequestStatus status;
  try {
    const auto reply =
        Request(config, {}, body, stop, {}, request.refresh_token,
                "application/x-www-form-urlencoded", false, &status);
    const auto json = Parse(reply);
    RefreshedCredential credential;
    credential.access_token = String(json, "/access_token");
    if (credential.access_token.empty())
      throw ProviderError("ChatGPT token refresh returned no access token");
    credential.refresh_token =
        String(json, "/refresh_token", request.refresh_token);
    credential.id_token = String(json, "/id_token");
    credential.has_scope = json["/scope"].exists();
    credential.scope = String(json, "/scope");
    credential.expires_in = Number(json, "/expires_in", 3600);
    auto type = String(json, "/token_type", "Bearer");
    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (type != "bearer")
      throw ProviderError(
          "ChatGPT token refresh returned an unsupported token type");
    return credential;
  } catch (const ProviderError& error) {
    ProviderError failure(
        "ChatGPT token refresh failed: " + std::string(error.what()),
        error.retryable, status.http, error.retry_after, error.code,
        status.curl);
    config.auth = ProviderAuth::ChatGPT;
    LogFailure("Token refresh failed", config, failure);
    throw failure;
  }
}

auto RevokeCredential(std::string_view endpoint, std::string_view client_id,
                      std::string_view refresh_token, std::stop_token stop)
    -> void {
  if (endpoint.empty() || client_id.empty() || refresh_token.empty()) {
    throw std::runtime_error("ChatGPT token revocation is not configured");
  }
  ProviderConfig config;
  config.api = ProviderAPI::OpenAIResponses;
  config.auth = ProviderAuth::None;
  config.base_url = endpoint;
  const auto body =
      "token=" + FormEscape(refresh_token) +
      "&token_type_hint=refresh_token&client_id=" + FormEscape(client_id);
  try {
    Request(config, {}, body, stop, {}, {}, "application/x-www-form-urlencoded",
            true);
  } catch (const ProviderError& error) {
    throw ProviderError("ChatGPT token revocation failed (HTTP " +
                            std::to_string(error.status) + ')',
                        error.retryable, error.status, error.retry_after);
  }
}

auto SerializeMessages(const std::vector<Message>& messages) -> std::string {
  std::vector<std::string> result;
  for (const auto& message : messages) {
    std::vector<std::string> content;
    for (const auto& block : message.content) {
      Object value{{"type", Quote(block.type)}};
      if (block.type == "tool_use") {
        value["id"] = Quote(block.id);
        value["name"] = Quote(block.name);
        value["input"] = Quote(block.input);
      } else if (block.type == "tool_result") {
        value["tool_use_id"] = Quote(block.id);
        value["content"] = Quote(block.text);
        value["is_error"] = block.is_error ? "true" : "false";
      } else if (block.type == "web_search") {
        value["id"] = Quote(block.id);
        value["query"] = Quote(block.text);
        std::vector<std::string> sources;
        for (const auto& source : block.sources)
          sources.push_back(Encode(
              {{"url", Quote(source.url)}, {"title", Quote(source.title)}}));
        value["sources"] = json::Encode(sources);
        if (!block.error.empty()) value["error"] = Quote(block.error);
      } else {
        value["text"] = Quote(block.text);
      }
      if (!block.images.empty()) {
        std::vector<std::string> images;
        for (const auto& image : block.images)
          images.push_back(
              Encode(Object{{"media_type", Quote(image.media_type)},
                            {"data", Quote(image.data)}}));
        value["images"] = json::Encode(images);
      }
      if (!block.resource_ids.empty())
        value["resource_ids"] = Strings(block.resource_ids);
      content.push_back(Encode(value));
    }
    Object value{{"role", Quote(message.role)},
                 {"content", json::Encode(content)}};
    if (!message.display.empty()) value["display"] = Quote(message.display);
    if (message.context_summary) value["contextSummary"] = "true";
    if (!message.provider_content.empty()) {
      (void)Items(Parse(message.provider_content));
      value["providerContent"] = message.provider_content;
      value["providerApi"] =
          Quote(message.provider_api == ProviderAPI::AnthropicMessages
                    ? "anthropicMessages"
                    : "openAIResponses");
    }
    result.push_back(Encode(value));
  }
  return json::Encode(result);
}

auto ParseMessages(std::string_view json) -> std::vector<Message> {
  std::vector<Message> messages;
  for (const auto& item : Items(Parse(json))) {
    Message message;
    message.role = String(item, "/role");
    if (message.role != "user" && message.role != "assistant")
      throw std::runtime_error("invalid conversation role");
    message.display = String(item, "/display");
    message.context_summary = Boolean(item, "/contextSummary");
    if (item["/providerContent"].exists()) {
      (void)Items(item, "/providerContent");
      message.provider_content = Raw(item, "/providerContent");
      message.provider_api = String(item, "/providerApi") == "openAIResponses"
                                 ? ProviderAPI::OpenAIResponses
                                 : ProviderAPI::AnthropicMessages;
    }
    for (const auto& part : Items(item, "/content")) {
      ContentBlock block;
      block.type = String(part, "/type");
      if (block.type == "tool_use") {
        block.id = String(part, "/id");
        block.name = String(part, "/name");
        block.input = String(part, "/input", "{}");
      } else if (block.type == "tool_result") {
        block.id = String(part, "/tool_use_id");
        block.text = String(part, "/content");
        block.is_error = Boolean(part, "/is_error");
      } else if (block.type == "web_search") {
        block.id = String(part, "/id");
        block.text = String(part, "/query");
        block.error = String(part, "/error");
        for (const auto& source : Items(part, "/sources"))
          block.sources.push_back(
              {String(source, "/url"), String(source, "/title")});
      } else if (block.type == "text") {
        block.text = String(part, "/text");
      } else {
        throw std::runtime_error("invalid conversation content block");
      }
      for (const auto& image : Items(part, "/images"))
        block.images.push_back(
            {String(image, "/media_type"), String(image, "/data")});
      for (const auto& resource : Items(part, "/resource_ids"))
        block.resource_ids.push_back(String(resource, ""));
      message.content.push_back(std::move(block));
    }
    messages.push_back(std::move(message));
  }
  return messages;
}

}  // namespace tsugumi::agent
