#include "agent/wire.hh"


#include "tsugumi/util/json_object.hh"
#include "agent/presentation.hh"

namespace tsugumi::agent::wire {
namespace A = Tsugumi::Agent;
namespace {
auto Text(const flatbuffers::String *value) -> std::string {
  return value ? value->str() : "";
}
auto Messages(std::string_view stored)
    -> std::vector<std::unique_ptr<A::ModelMessageT>> {
  std::vector<std::unique_ptr<A::ModelMessageT>> out;
  for (const auto &raw : json::Array(stored)) {
    const auto o = json::Object(raw);
    auto m = std::make_unique<A::ModelMessageT>();
    m->role = json::Text(o, "role");
    m->content_json = json::Get(o, "content", "[]");
    m->display = json::Text(o, "display");
    m->context_summary = json::Bool(json::Get(o, "contextSummary"));
    if (o.contains("providerContent"))
      m->provider_content_json = json::Get(o, "providerContent");
    m->provider_api = json::Text(o, "providerApi");
    out.push_back(std::move(m));
  }
  return out;
}
} // namespace

auto ProviderJson(const Tsugumi::Host::ProviderInput& p) -> std::string {
  json::Members values{{"model", json::Quote(Text(p.model()))},
                       {"maxTokens", std::to_string(p.max_tokens())},
                       {"contextLimit", std::to_string(p.context_limit())}};
  if (p.endpoint()) values["baseURL"] = json::Quote(p.endpoint()->str());
  return json::Encode(values);
}
auto SelectionJson(const Tsugumi::Service::ProviderSelection& p)
    -> std::string {
  return json::Encode(
      json::Members{{"profile", json::Quote(Text(p.profile()))},
                    {"provider_id", json::Quote(Text(p.provider_id()))},
                    {"account_profile", json::Quote(Text(p.account_profile()))},
                    {"key_profile", json::Quote(Text(p.key_profile()))},
                    {"model", json::Quote(Text(p.model()))},
                    {"purpose", json::Quote(Text(p.purpose()))},
                    {"use_choices", p.use_choices() ? "true" : "false"}});
}
auto Usage(std::string_view stored) -> Tsugumi::Service::UsageT {
  const auto o = json::Object(stored);
  Tsugumi::Service::UsageT out;
  out.input_tokens = json::UInt(json::Get(o, "inputTokens"));
  out.output_tokens = json::UInt(json::Get(o, "outputTokens"));
  out.cache_read_tokens = json::UInt(json::Get(o, "cacheReadTokens"));
  out.cache_write_tokens = json::UInt(json::Get(o, "cacheWriteTokens"));
  return out;
}
auto Conversation(std::string_view stored) -> A::ConversationT {
  const auto o = json::Object(stored);
  A::ConversationT out;
  out.id = json::Text(o, "id");
  out.title = json::Text(o, "title");
  out.updated_at = json::UInt(json::Get(o, "updatedAt"));
  out.messages = Messages(json::Get(o, "messages", "[]"));
  if (const auto text = json::Text(o, "streaming"); !text.empty()) {
    auto streamed = std::make_unique<A::ModelMessageT>();
    streamed->role = "assistant";
    streamed->content_json =
        json::Encode(std::vector<std::string>{json::Encode(json::Members{
            {"type", json::Quote("text")}, {"text", json::Quote(text)}})});
    out.messages.push_back(std::move(streamed));
  }
  out.archive = Messages(json::Get(o, "archive", "[]"));
  out.summary = json::Text(o, "summary");
  out.usage = std::make_unique<Tsugumi::Service::UsageT>(Usage(json::Get(o, "usage", "{}")));
  for (const auto &raw : json::Array(json::Get(o, "tools", "[]"))) {
    const auto t = json::Object(raw);
    auto tool = std::make_unique<A::ToolRecordT>();
    tool->id = json::Text(t, "id");
    tool->name = json::Text(t, "name");
    tool->arguments_json = json::Text(t, "arguments");
    tool->op_id = json::Text(t, "opId");
    tool->state = json::Text(t, "state");
    tool->result_json = json::Text(t, "result");
    tool->is_error = json::Bool(json::Get(t, "isError"));
    out.tools.push_back(std::move(tool));
  }
  for (const auto &raw : json::Array(json::Get(o, "helpers", "[]"))) {
    const auto h = json::Object(raw);
    auto helper = std::make_unique<A::HelperT>();
    helper->call_id = json::Text(h, "call_id");
    helper->task = json::UInt(json::Get(h, "task"));
    // What compaction took out of the helper's context stays readable: the
    // earlier messages come first, then the ones the model still holds.
    helper->messages = Messages(json::Get(h, "archive", "[]"));
    for (auto& message : Messages(json::Get(h, "messages", "[]")))
      helper->messages.push_back(std::move(message));
    if (const auto text = json::Text(h, "streaming"); !text.empty()) {
      auto streamed = std::make_unique<A::ModelMessageT>();
      streamed->role = "assistant";
      streamed->content_json =
          json::Encode(std::vector<std::string>{json::Encode(json::Members{
              {"type", json::Quote("text")}, {"text", json::Quote(text)}})});
      helper->messages.push_back(std::move(streamed));
    }
    const auto result = json::Get(h, "result", "null");
    if (result != "null")
      helper->result_json = result;
    helper->state = json::Text(h, "state");
    helper->spec_json = json::Get(h, "spec", "{}");
    helper->anchor = json::UInt(json::Get(h, "anchor"));
    out.helpers.push_back(std::move(helper));
  }
  out.rows = Presentation(out);
  return out;
}
auto Event(std::string_view stored) -> A::EventT {
  const auto o = json::Object(stored);
  A::EventT out;
  out.kind = json::Text(o, "kind");
  out.text = json::Text(o, "text");
  out.message = json::Text(o, "message");
  out.input_json = json::Text(o, "input");
  out.id = json::Text(o, "id");
  out.name = json::Text(o, "name");
  out.ok = json::Bool(json::Get(o, "ok", "true"));
  out.index = json::UInt(json::Get(o, "index"));
  for (const auto &raw : json::Array(json::Get(o, "asked", "[]")))
    out.asked.push_back(json::String(raw));
  out.groups = AccessGroups(out.asked);
  if (o.contains("event"))
    out.event = std::make_unique<A::EventT>(Event(json::Get(o, "event")));
  return out;
}
} // namespace tsugumi::agent::wire
