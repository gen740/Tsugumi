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

#include "schema.hh"

namespace tsugumi::agent {
namespace {

using Json = hosq::json::GenericDom<>;
using Object = std::map<std::string, std::string>;
constexpr size_t kMaxResponseBytes = 64u << 20;
constexpr size_t kMaxEventBytes = 16u << 20;

auto Parse(std::string_view text) -> Json { return schema::capture(text); }

auto Quote(std::string_view text) -> std::string {
  schema::v::String value;
  value.value() = text;
  return schema::encode(value);
}

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

auto Encode(const Object& object) -> std::string {
  std::string text = "{";
  for (const auto& [key, value] : object) {
    if (text.size() > 1) text += ',';
    text += Quote(key) + ':' + value;
  }
  return text + '}';
}

auto Array(const std::vector<std::string>& values) -> std::string {
  std::string text = "[";
  for (const auto& value : values) {
    if (text.size() > 1) text += ',';
    text += value;
  }
  return text + ']';
}

auto Strings(const std::vector<std::string>& values) -> std::string {
  std::vector<std::string> strings;
  strings.reserve(values.size());
  for (const auto& value : values) strings.push_back(Quote(value));
  return Array(strings);
}

auto TextBlock(std::string_view text) -> std::string {
  return Encode({{"type", Quote("text")}, {"text", Quote(text)}});
}

auto ImageBlock(const Image& image, bool responses) -> std::string {
  if (responses) {
    return Encode({{"type", Quote("input_image")},
                   {"image_url", Quote("data:" + image.media_type + ";base64," +
                                       image.data)}});
  }
  return Encode({{"type", Quote("image")},
                 {"source", Encode({{"type", Quote("base64")},
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
    return Encode(
        {{"type", Quote("tool_result")},
         {"tool_use_id", Quote(block.id)},
         {"content", block.images.empty() ? Quote(block.text) : Array(content)},
         {"is_error", block.is_error ? "true" : "false"}});
  }
  return TextBlock(block.type == "web_search" ? SearchText(block) : block.text);
}

auto OutputLimit(const ProviderConfig& config) -> uint32_t {
  return config.effort == "xhigh" || config.effort == "max"
             ? std::max(config.max_tokens, 64000u)
             : config.max_tokens;
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
    wired.push_back(
        Encode({{"role", Quote(message.role)}, {"content", Array(content)}}));
  }
  Object body{
      {"model", Quote(config.model)},
      {"max_tokens", std::to_string(OutputLimit(config))},
      {"stream", "true"},
      {"messages", Array(wired)},
      {"system",
       Array({Encode({{"type", Quote("text")},
                      {"text", Quote(system)},
                      {"cache_control", "{\"type\":\"ephemeral\"}"}})})}};
  if (!config.effort.empty())
    body["output_config"] = Encode({{"effort", Quote(config.effort)}});
  if (config.fast) body["speed"] = Quote("fast");
  std::vector<std::string> definitions;
  for (const auto& tool : tools) {
    if (!ValidToolArguments(tool.input_schema))
      throw std::runtime_error("invalid tool input schema");
    definitions.push_back(Encode({{"name", Quote(tool.name)},
                                  {"description", Quote(tool.description)},
                                  {"input_schema", tool.input_schema},
                                  {"eager_input_streaming", "true"}}));
  }
  if (config.web_search && config.auth != ProviderAuth::None) {
    definitions.push_back(
        "{\"type\":\"web_search_20250305\",\"name\":\"web_"
        "search\",\"max_uses\":5}");
  }
  if (!definitions.empty()) {
    auto last = Members(Parse(definitions.back()));
    last["cache_control"] = "{\"type\":\"ephemeral\"}";
    definitions.back() = Encode(last);
    body["tools"] = Array(definitions);
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
      input.push_back(
          Encode({{"role", Quote(message.role)}, {"content", Array(text)}}));
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
        input.push_back(
            Encode({{"type", Quote("function_call_output")},
                    {"call_id", Quote(block.id)},
                    {"output", block.images.empty() ? Quote(block.text)
                                                    : Array(output)}}));
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
  return Array(input);
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
  if (!plan) body["max_output_tokens"] = std::to_string(OutputLimit(config));
  if (!config.effort.empty())
    body["reasoning"] = Encode({{"effort", Quote(config.effort)}});
  if (config.fast) body["service_tier"] = Quote("fast");
  std::vector<std::string> functions;
  for (const auto& tool : tools) {
    if (!ValidToolArguments(tool.input_schema))
      throw std::runtime_error("invalid tool input schema");
    functions.push_back(Encode({{"type", Quote("function")},
                                {"name", Quote(tool.name)},
                                {"description", Quote(tool.description)},
                                {"parameters", tool.input_schema},
                                {"strict", "false"}}));
  }
  std::vector<std::string> definitions;
  if (plan && !functions.empty()) {
    definitions.push_back(
        Encode({{"type", Quote("namespace")},
                {"name", Quote("tsugumi")},
                {"description", Quote("Authorized Tsugumi operations.")},
                {"tools", Array(functions)}}));
  } else {
    definitions = std::move(functions);
  }
  if (config.web_search && config.auth != ProviderAuth::None) {
    definitions.push_back("{\"type\":\"web_search\"}");
    if (!plan) body["include"] = "[\"web_search_call.action.sources\"]";
  }
  if (!definitions.empty()) body["tools"] = Array(definitions);
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
            if (line.substr(0, colon) == "retry-after")
              delay = std::min(delay, uint64_t{120}) * 1000;
            self.retry_after =
                std::chrono::milliseconds(std::min(delay, uint64_t{120000}));
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
             bool allow_no_content = false) -> std::string {
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
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT,
                   static_cast<long>(config.timeout_seconds));
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
  if (transfer.error) std::rethrow_exception(transfer.error);
  if (stop.stop_requested()) throw Cancelled();
  if (code != CURLE_OK) {
    const bool transient =
        code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST ||
        code == CURLE_RECV_ERROR || code == CURLE_SEND_ERROR ||
        code == CURLE_PARTIAL_FILE || code == CURLE_OPERATION_TIMEDOUT ||
        code == CURLE_GOT_NOTHING || code == CURLE_HTTP2;
    throw ProviderError(
        std::string("model connection failed: ") + curl_easy_strerror(code),
        transient);
  }
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &transfer.status);
  if (transfer.status != 200 && !(allow_no_content && transfer.status == 204)) {
    std::string detail;
    std::string error_code;
    try {
      const auto error = Parse(transfer.body);
      detail = String(error, "/error/message");
      error_code = String(error, "/error/code",
                          String(error, "/error/type", String(error, "/code")));
    } catch (...) {
    }
    if (config.auth == ProviderAuth::ChatGPT &&
        error_code == "subscription_sharing_usage_limit_exceeded")
      throw ProviderError(
          "ChatGPT usage limit reached: review your plan or this app's limit "
          "in ChatGPT settings",
          false, static_cast<int>(transfer.status));
    if (detail.empty()) detail = "request refused";
    if (!credential.empty()) {
      for (size_t at = detail.find(credential); at != std::string::npos;
           at = detail.find(credential, at + 10)) {
        detail.replace(at, credential.size(), "[redacted]");
      }
    }
    // Do not persist a raw HTTP response: providers may echo credentials or
    // entire prompts. Only their bounded error message belongs in UI events.
    throw ProviderError(
        "model returned HTTP " + std::to_string(transfer.status) + ": " +
            detail.substr(0, 400),
        transfer.status == 429 || transfer.status >= 500,
        static_cast<int>(transfer.status), transfer.retry_after);
  }
  return transfer.body;
}

auto WithCredential(const ProviderConfig& config,
                    const std::function<std::string(std::string_view)>& request)
    -> std::string {
  const auto credential =
      config.auth == ProviderAuth::ChatGPT && config.access_token
          ? config.access_token({})
          : config.credential;
  try {
    return request(credential);
  } catch (const ProviderError& error) {
    if (error.status != 401 || config.auth != ProviderAuth::ChatGPT ||
        !config.access_token)
      throw;
    return request(config.access_token(credential));
  }
}

struct ModelStream {
  ProviderAPI api;
  const std::function<void(const ModelEvent&)>& event;
  ModelReply reply;
  struct Block {
    Object value;
    std::string arguments;
    bool has_arguments = false;
    bool closed = false;
  };
  std::map<uint32_t, Block> blocks;
  std::map<uint32_t, std::string> output;
  std::map<uint32_t, ContentBlock> calls;
  std::string text;
  bool finished = false;

  auto Emit(ModelEvent value) const -> void {
    if (event) event(value);
  }

  auto AppendField(Block& block, std::string_view key, std::string_view delta)
      -> void {
    auto current = block.value.find(std::string(key));
    std::string value =
        current == block.value.end() ? "" : String(Parse(current->second), "");
    value += delta;
    block.value[std::string(key)] = Quote(value);
  }

  auto Consume(std::string_view event_name, std::string_view data) -> void {
    const auto json = Parse(data);
    const auto type = String(json, "/type", std::string(event_name));
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
      const auto kind = String(json, "/content_block/type");
      if (kind == "text") {
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
        if (String(Parse(Encode(block.value)), "/type") == "tool_use") {
          Emit({"toolInput",
                delta,
                {},
                String(Parse(Encode(block.value)), "/name"),
                index});
        }
      } else if (kind == "thinking_delta") {
        AppendField(block, "thinking", String(json, "/delta/thinking"));
      } else if (kind == "signature_delta") {
        AppendField(block, "signature", String(json, "/delta/signature"));
      } else if (kind == "citations_delta") {
        auto citations = block.value.contains("citations")
                             ? Items(Parse(block.value["citations"]))
                             : std::vector<Json>{};
        std::vector<std::string> items;
        for (const auto& item : citations) items.push_back(schema::text(item));
        items.push_back(Raw(json, "/delta/citation", "{}"));
        block.value["citations"] = Array(items);
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
      const auto value = Parse(Encode(block.value));
      if (String(value, "/type") == "server_tool_use") {
        Emit({"searchStarted", String(value, "/input/query"),
              String(value, "/id"), "web_search", index});
      } else if (String(value, "/type") == "web_search_tool_result") {
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
      const auto code = String(json, "/error/type");
      throw ProviderError(String(json, "/error/message", "model stream failed"),
                          code == "overloaded_error" || code == "api_error" ||
                              code == "rate_limit_error");
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
      Emit({"toolInput", delta, it->second.id, it->second.name, index});
    } else if (type == "response.function_call_arguments.done") {
      auto it = calls.find(index);
      if (it == calls.end())
        throw ProviderError("model arguments have no tool call");
      const auto complete = String(json, "/arguments");
      if (!it->second.input.empty() && it->second.input != complete)
        throw ProviderError("model arguments changed after streaming");
      it->second.input = complete;
    } else if (type == "response.output_item.done") {
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
      // The terminal response is authoritative, including encrypted reasoning.
      if (json["/response/output"].exists()) {
        output.clear();
        uint32_t i = 0;
        for (const auto& item : Items(json, "/response/output"))
          output[i++] = schema::text(item);
      }
      finished = true;
    } else if (type == "response.failed" || type == "error") {
      const auto code =
          String(json, "/response/error/code", String(json, "/error/code"));
      if (code == "subscription_sharing_usage_limit_exceeded")
        throw ProviderError(
            "ChatGPT usage limit reached: review your plan or this app's limit "
            "in ChatGPT settings");
      throw ProviderError(
          String(json, "/response/error/message",
                 String(json, "/error/message", "model stream failed")),
          code == "server_error" || code == "rate_limit_exceeded");
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
        const auto value = Parse(Encode(block.value));
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
    if (!original.empty()) reply.message.provider_content = Array(original);
    std::set<std::string> ids;
    for (const auto& block : reply.message.content) {
      if (block.type == "tool_use" && (block.id.empty() || block.name.empty() ||
                                       !ids.insert(block.id).second)) {
        throw ProviderError("model returned an invalid or duplicate tool call");
      }
    }
    return std::move(reply);
  }
};

}  // namespace

ProviderError::ProviderError(std::string message, bool retryable_value,
                             int status_value,
                             std::chrono::milliseconds retry_after_value)
    : std::runtime_error(std::move(message)),
      retryable(retryable_value),
      status(status_value),
      retry_after(retry_after_value) {}

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
                 const std::function<void(const ModelEvent&)>& event)
    -> ModelReply {
  if (config.model.empty()) throw std::runtime_error("model is not configured");
  const bool anthropic = config.api == ProviderAPI::AnthropicMessages;
  const auto body = anthropic ? MessagesBody(config, system, messages, tools)
                              : ResponsesBody(config, system, messages, tools);
  std::optional<ModelReply> reply;
  WithCredential(config, [&](std::string_view credential) {
    ModelStream stream{config.api, event, {}, {}, {}, {}, {}, false};
    SSE sse([&](std::string_view name, std::string_view data) {
      stream.Consume(name, data);
    });
    Request(
        config, anthropic ? "v1/messages" : "v1/responses", body, stop,
        [&](std::string_view data) { sse.Feed(data); }, credential);
    sse.Finish();
    reply = stream.Finish();
    return std::string{};
  });
  return std::move(*reply);
}

auto ListModels(const ProviderConfig& config, std::stop_token stop)
    -> std::vector<std::pair<std::string, std::string>> {
  const auto body = WithCredential(config, [&](std::string_view credential) {
    return Request(config, "v1/models", std::nullopt, stop, {}, credential);
  });
  std::vector<std::pair<std::string, std::string>> models;
  for (const auto& item : Items(Parse(body), "/data")) {
    const auto id = String(item, "/id");
    if (!id.empty()) models.emplace_back(id, String(item, "/display_name", id));
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
  config.timeout_seconds = 15;
  std::string reply;
  try {
    reply = Request(config, {}, body, stop, {}, {},
                    "application/x-www-form-urlencoded");
  } catch (const ProviderError& error) {
    // The authorization server's response may echo a submitted token.
    throw ProviderError("ChatGPT token refresh failed (HTTP " +
                            std::to_string(error.status) + ')',
                        error.retryable, error.status, error.retry_after);
  }
  const auto json = Parse(reply);
  RefreshedCredential credential;
  credential.access_token = String(json, "/access_token");
  if (credential.access_token.empty())
    throw ProviderError("ChatGPT token refresh returned no access token");
  credential.refresh_token =
      String(json, "/refresh_token", request.refresh_token);
  credential.id_token = String(json, "/id_token");
  credential.scope = String(json, "/scope");
  credential.expires_in = Number(json, "/expires_in", 3600);
  auto type = String(json, "/token_type", "Bearer");
  std::transform(type.begin(), type.end(), type.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (type != "bearer")
    throw ProviderError(
        "ChatGPT token refresh returned an unsupported token type");
  if (!credential.scope.empty()) {
    const auto scopes = ' ' + credential.scope + ' ';
    if (scopes.find(" chatgpt.tokens.use.direct ") == std::string::npos) {
      throw ProviderError(
          "The ChatGPT plan is not enabled for this registration");
    }
  }
  return credential;
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
  config.timeout_seconds = 15;
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
        value["sources"] = Array(sources);
        if (!block.error.empty()) value["error"] = Quote(block.error);
      } else {
        value["text"] = Quote(block.text);
      }
      if (!block.images.empty()) {
        std::vector<std::string> images;
        for (const auto& image : block.images)
          images.push_back(Encode({{"media_type", Quote(image.media_type)},
                                   {"data", Quote(image.data)}}));
        value["images"] = Array(images);
      }
      if (!block.resource_ids.empty())
        value["resource_ids"] = Strings(block.resource_ids);
      content.push_back(Encode(value));
    }
    Object value{{"role", Quote(message.role)}, {"content", Array(content)}};
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
  return Array(result);
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
