#pragma once
// Test-only projections for content/history assertions and failure diagnostics.
// The product clients consume generated typed replies directly.
#include "tsugumi/protocol/agent_generated.h"
#include "tsugumi/protocol/host_generated.h"
#include <type_traits>

namespace tsugumi::test {
namespace A = Tsugumi::Agent;
namespace H = Tsugumi::Host;
// Optional FlatBuffers vectors represent an empty list when omitted.
template<class T> auto ListSize(const flatbuffers::Vector<T>* values) -> size_t {
  return values ? values->size() : 0;
}
inline auto View(const flatbuffers::String *s) -> std::string {
  return Quote(s ? s->str() : "");
}
template <class T>
  requires std::is_arithmetic_v<T>
auto View(T value) -> std::string {
  if constexpr (std::is_same_v<T, bool>)
    return value ? "true" : "false";
  else
    return std::to_string(value);
}
inline auto View(A::Access v) -> std::string {
  return Quote(A::EnumNameAccess(v));
}
inline auto View(A::GrantScope v) -> std::string {
  return Quote(A::EnumNameGrantScope(v));
}
inline auto
Object(std::initializer_list<std::pair<std::string, std::string>> fields)
    -> std::string {
  std::string out = "{";
  for (const auto &[key, value] : fields) {
    if (out.size() > 1)
      out += ',';
    out += Quote(key) + ':' + value;
  }
  return out + '}';
}
inline auto View(const Tsugumi::Service::HelloResult *value) -> std::string;
inline auto View(const Tsugumi::Service::Failure *value) -> std::string;
inline auto View(const Tsugumi::Service::Acknowledgement *value) -> std::string;
inline auto View(const A::SkillReference *value) -> std::string;
inline auto View(const A::ModelMessage *value) -> std::string;
inline auto View(const A::Usage *value) -> std::string;
inline auto View(const A::ToolRecord *value) -> std::string;
inline auto View(const A::Helper *value) -> std::string;
inline auto View(const A::Conversation *value) -> std::string;
inline auto View(const A::ConversationResult *value) -> std::string;
inline auto View(const A::ConversationEntry *value) -> std::string;
inline auto View(const A::ConversationList *value) -> std::string;
inline auto View(const A::SearchMatch *value) -> std::string;
inline auto View(const A::SearchResult *value) -> std::string;
inline auto View(const A::Event *value) -> std::string;
inline auto View(const A::SequencedEvent *value) -> std::string;
inline auto View(const A::EventsPage *value) -> std::string;
inline auto View(const A::Tool *value) -> std::string;
inline auto View(const A::ToolCatalog *value) -> std::string;
inline auto View(const A::Image *value) -> std::string;
inline auto View(const A::ToolResult *value) -> std::string;
inline auto View(const A::AccessResult *value) -> std::string;
inline auto View(const A::AccessEntry *value) -> std::string;
inline auto View(const A::Grant *value) -> std::string;
inline auto View(const A::AccessList *value) -> std::string;
inline auto View(const A::DemoList *value) -> std::string;
inline auto View(const A::TextResult *value) -> std::string;
inline auto View(const H::CredentialToken *value) -> std::string;
inline auto View(const H::ProjectDefaults *value) -> std::string;
inline auto View(const H::ProjectInfo *value) -> std::string;
inline auto View(const H::ProjectLocation *value) -> std::string;
inline auto View(const H::RecentProject *value) -> std::string;
inline auto View(const H::RecentList *value) -> std::string;
inline auto View(const H::Demo *value) -> std::string;
inline auto View(const H::DemoList *value) -> std::string;
inline auto View(const H::Model *value) -> std::string;
inline auto View(const H::ModelList *value) -> std::string;
template <class T>
auto View(const flatbuffers::Vector<T> *values) -> std::string {
  std::string out = "[";
  if (values)
    for (const auto value : *values) {
      if (out.size() > 1)
        out += ',';
      out += View(value);
    }
  return out + ']';
}
inline auto View(const Tsugumi::Service::HelloResult *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"protocol", View(value->protocol())},
                 {"server_id", View(value->server_id())},
                 {"principal", View(value->principal())}});
}
inline auto View(const Tsugumi::Service::Failure *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"code", View(value->code())},
                 {"message", View(value->message())},
                 {"request_id", View(value->request_id())}});
}
inline auto View(const Tsugumi::Service::Acknowledgement *value)
    -> std::string {
  if (!value)
    return "null";
  return Object({{"accepted", View(value->accepted())},
                 {"revoked", View(value->revoked())}});
}
inline auto View(const A::SkillReference *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"name", View(value->name())},
                 {"resource_id", View(value->resource_id())},
                 {"path", View(value->path())}});
}
inline auto View(const A::ModelMessage *value) -> std::string {
  if (!value)
    return "null";
  return Object(
      {{"role", View(value->role())},
       {"content",
        (value->content_json() ? value->content_json()->str() : "null")},
       {"display", View(value->display())},
       {"contextSummary", View(value->context_summary())},
       {"providerContent", value->provider_content_json()
                               ? value->provider_content_json()->str()
                               : "null"},
       {"providerApi", View(value->provider_api())}});
}
inline auto View(const A::Usage *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"inputTokens", View(value->input_tokens())},
                 {"outputTokens", View(value->output_tokens())},
                 {"cacheReadTokens", View(value->cache_read_tokens())},
                 {"cacheWriteTokens", View(value->cache_write_tokens())}});
}
inline auto View(const A::ToolRecord *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"id", View(value->id())},
                 {"name", View(value->name())},
                 {"arguments", View(value->arguments_json())},
                 {"opId", View(value->op_id())},
                 {"state", View(value->state())},
                 {"result", View(value->result_json())},
                 {"isError", View(value->is_error())}});
}
inline auto View(const A::Helper *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"call_id", View(value->call_id())},
                 {"task", View(value->task())},
                 {"messages", View(value->messages())},
                 {"result", (value->result_json() ? value->result_json()->str()
                                                  : "null")},
                 {"state", View(value->state())}});
}
inline auto View(const A::Conversation *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"id", View(value->id())},
                 {"title", View(value->title())},
                 {"updatedAt", View(value->updated_at())},
                 {"messages", View(value->messages())},
                 {"archive", View(value->archive())},
                 {"summary", View(value->summary())},
                 {"tools", View(value->tools())},
                 {"helpers", View(value->helpers())},
                 {"usage", View(value->usage())}});
}
inline auto View(const A::ConversationResult *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"conversation", View(value->conversation())},
                 {"busy", View(value->busy())},
                 {"resumable", View(value->resumable())},
                 {"continuable", View(value->continuable())},
                 {"project_id", View(value->project_id())},
                 {"event_seq", View(value->event_seq())}});
}
inline auto View(const A::ConversationEntry *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"id", View(value->id())},
                 {"project_id", View(value->project_id())},
                 {"title", View(value->title())},
                 {"updatedAt", View(value->updated_at())},
                 {"busy", View(value->busy())}});
}
inline auto View(const A::ConversationList *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"conversations", View(value->conversations())}});
}
inline auto View(const A::SearchMatch *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"session", View(value->session())},
                 {"title", View(value->title())},
                 {"snippet", View(value->snippet())},
                 {"updatedAt", View(value->updated_at())}});
}
inline auto View(const A::SearchResult *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"matches", View(value->matches())}});
}
inline auto View(const A::Event *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"kind", View(value->kind())},
                 {"text", View(value->text())},
                 {"message", View(value->message())},
                 {"input", View(value->input_json())},
                 {"id", View(value->id())},
                 {"name", View(value->name())},
                 {"ok", View(value->ok())},
                 {"index", View(value->index())},
                 {"asked", View(value->asked())},
                 {"event", View(value->event())}});
}
inline auto View(const A::SequencedEvent *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"seq", View(value->seq())}, {"event", View(value->event())}});
}
inline auto View(const A::EventsPage *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"events", View(value->events())},
                 {"next_seq", View(value->next_seq())},
                 {"busy", View(value->busy())}});
}
inline auto View(const A::Tool *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"name", View(value->name())},
                 {"description", View(value->description())},
                 {"inputSchema", (value->input_schema_json()
                                      ? value->input_schema_json()->str()
                                      : "null")}});
}
inline auto View(const A::ToolCatalog *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"tools", View(value->tools())}});
}
inline auto View(const A::Image *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"media_type", View(value->media_type())},
                 {"data", View(value->data())}});
}
inline auto View(const A::ToolResult *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"text", View(value->text())},
                 {"isError", View(value->is_error())},
                 {"images", View(value->images())}});
}
inline auto View(const A::AccessResult *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"id", View(value->id())}});
}
inline auto View(const A::AccessEntry *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"id", View(value->id())},
                 {"project_id", View(value->project_id())},
                 {"conversation_id", View(value->conversation_id())},
                 {"access", View(value->access())},
                 {"scope", View(value->scope())},
                 {"reason", View(value->reason())},
                 {"state", View(value->state())}});
}
inline auto View(const A::Grant *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"id", View(value->id())},
                 {"project_id", View(value->project_id())},
                 {"conversation_id", View(value->conversation_id())},
                 {"access", View(value->access())},
                 {"active", View(value->active())}});
}
inline auto View(const A::AccessList *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"requests", View(value->requests())},
                 {"grants", View(value->grants())}});
}
inline auto View(const A::DemoList *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"paths", View(value->paths())}});
}
inline auto View(const A::TextResult *value) -> std::string {
  if (!value)
    return "null";
  return Object(
      {{"text", View(value->text())}, {"usage", View(value->usage())}});
}
inline auto View(const H::CredentialToken *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"access_token", View(value->access_token())},
                 {"client_id", View(value->client_id())}});
}
inline auto View(const H::ProjectDefaults *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"nixpkgsPin", View(value->nixpkgs_pin())}});
}
inline auto View(const H::ProjectInfo *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"path", View(value->path())}, {"name", View(value->name())}});
}
inline auto View(const H::ProjectLocation *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"path", View(value->path())}});
}
inline auto View(const H::RecentProject *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"path", View(value->path())},
                 {"name", View(value->name())},
                 {"openedAt", View(value->opened_at())}});
}
inline auto View(const H::RecentList *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"projects", View(value->projects())}});
}
inline auto View(const H::Demo *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"id", View(value->id())},
                 {"path", View(value->path())},
                 {"title", View(value->title())},
                 {"description", View(value->description())},
                 {"tags", View(value->tags())},
                 {"field", View(value->field())}});
}
inline auto View(const H::DemoList *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"demos", View(value->demos())}});
}
inline auto View(const H::Model *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"id", View(value->id())}, {"name", View(value->name())}});
}
inline auto View(const H::ModelList *value) -> std::string {
  if (!value)
    return "null";
  return Object({{"models", View(value->models())}});
}
} // namespace tsugumi::test
