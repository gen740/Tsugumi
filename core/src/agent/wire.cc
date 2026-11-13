#include "agent/wire.hh"

#include "agent/json.hh"

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

auto ProviderJson(const Tsugumi::Service::Provider &p) -> std::string {
  json::Members values{{"provider", json::Quote(Text(p.provider()))},
                       {"api", json::Quote(Text(p.api()))},
                       {"auth", json::Quote(Text(p.auth()))},
                       {"model", json::Quote(Text(p.model()))},
                       {"credential", json::Quote(Text(p.credential()))},
                       {"credential_id", json::Quote(Text(p.credential_id()))},
                       {"effort", json::Quote(Text(p.effort()))},
                       {"fast", p.fast() ? "true" : "false"},
                       {"webSearch", p.web_search() ? "true" : "false"},
                       {"maxTokens", std::to_string(p.max_tokens())},
                       {"contextLimit", std::to_string(p.context_limit())}};
  if (p.base_url() && p.base_url()->size())
    values["baseURL"] = json::Quote(p.base_url()->str());
  return json::Encode(values);
}
auto RegistrationJson(const Tsugumi::Host::CredentialRegistration &p)
    -> std::string {
  return json::Encode(json::Members{
      {"client_id", json::Quote(Text(p.client_id()))},
      {"access_token", json::Quote(Text(p.access_token()))},
      {"refresh_token", json::Quote(Text(p.refresh_token()))},
      {"token_endpoint", json::Quote(Text(p.token_endpoint()))},
      {"revocation_endpoint", json::Quote(Text(p.revocation_endpoint()))},
      {"resource", json::Quote(Text(p.resource()))},
      {"expires_at_ms", std::to_string(p.expires_at_ms())},
      {"id_token", json::Quote(Text(p.id_token()))},
      {"scope", json::Quote(Text(p.scope()))}});
}
auto Usage(std::string_view stored) -> A::UsageT {
  const auto o = json::Object(stored);
  A::UsageT out;
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
  out.archive = Messages(json::Get(o, "archive", "[]"));
  out.summary = json::Text(o, "summary");
  out.usage = std::make_unique<A::UsageT>(Usage(json::Get(o, "usage", "{}")));
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
    helper->messages = Messages(json::Get(h, "messages", "[]"));
    const auto result = json::Get(h, "result", "null");
    if (result != "null")
      helper->result_json = result;
    helper->state = json::Text(h, "state");
    out.helpers.push_back(std::move(helper));
  }
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
  if (o.contains("event"))
    out.event = std::make_unique<A::EventT>(Event(json::Get(o, "event")));
  return out;
}
} // namespace tsugumi::agent::wire
