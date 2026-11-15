#include "host/models.hh"

#include "agent/json.hh"
#include "agent/wire.hh"
#include "host/failure.hh"
#include "host/settings.hh"

namespace tsugumi::host {
namespace H = Tsugumi::Host;
namespace json = agent::json;
namespace wire = agent::wire;
namespace {
auto Text(const flatbuffers::String* value) -> std::string {
  return value ? value->str() : "";
}
auto Human(bool human) -> void {
  if (!human)
    throw Failure("permission_denied",
                  "This operation requires the authenticated user");
}
}  // namespace
Models::Models(sql::Db& db, Settings& settings, std::stop_token stop,
               std::function<std::string(std::string)> scrub)
    : db(db), host_settings(&settings), stop(stop), scrub(std::move(scrub)) {}

auto Models::StoredChatGPTModels(const std::filesystem::path& state_dir)
    -> std::vector<std::string> {
  std::vector<std::string> models;
  sql::Db model_db((state_dir / "host.sqlite").string(), true);
  auto q =
      model_db.prepare("SELECT models FROM model_lists WHERE id='chatgpt'");
  if (q.step())
    for (const auto& row : json::Array(q.text(0)))
      models.push_back(json::Text(json::Object(row), "id"));
  return models;
}

auto Models::ContextWindowTokens(const std::filesystem::path& state_dir,
                                 std::string_view provider,
                                 std::string_view model) -> uint32_t {
  if (std::filesystem::exists(state_dir / "host.sqlite")) {
    sql::Db catalog((state_dir / "host.sqlite").string(), true);
    auto q = catalog.prepare("SELECT models FROM model_lists WHERE id=?");
    q.bind(1, std::string(provider));
    if (q.step())
      for (const auto& row : json::Array(q.text(0))) {
        const auto value = json::Object(row);
        if (json::Text(value, "id") == model) {
          const auto tokens =
              json::UInt(json::Get(value, "contextWindowTokens"));
          if (tokens) return tokens;
        }
      }
  }
  // Exact IDs offered in agent/Shared/agent.json. Unknown models deliberately
  // use 128K; the provider's context refusal remains the final safety net.
  // Sources: platform.claude.com/docs/en/build-with-claude/context-windows
  // and developers.openai.com/api/docs/models/<model-id>.
  for (const auto* id :
       {"claude-fable-5-1", "claude-mythos-5-1", "claude-fable-5",
        "claude-mythos-5", "claude-opus-5-5", "claude-opus-5",
        "claude-opus-4-8", "claude-opus-4-7", "claude-opus-4-6",
        "claude-sonnet-5-5", "claude-sonnet-5", "claude-sonnet-4-6",
        "claude-haiku-5-5", "claude-mythos-preview"})
    if (model == id) return 1000000;
  if (model == "claude-opus-4-5-20251101") return 200000;
  for (const auto* id : {"gpt-6.1-sol", "gpt-6.1-terra", "gpt-6-astra",
                         "gpt-6-sol", "gpt-6-luna", "gpt-5.6-sol",
                         "gpt-5.6-terra", "gpt-5.6-luna", "gpt-5.5"})
    if (model == id) return 1050000;
  return 128000;
}

auto Models::Handle(const RequestIn& request, bool human,
                    std::unique_lock<std::recursive_mutex>& lock)
    -> std::string {
  const auto* r = H::GetMessage(request.frame->data())->payload_as_Request();
  Human(human);
  const auto* p = r->value_as_ModelsListRequest();
  const auto key = p->provider_id()->str();
  if (key.empty())
    throw Failure("invalid_request", "A model catalog needs a provider ID");
  if (p->refresh()) {
    auto config = host_settings->Resolve(
        {{"profile", json::Quote(Text(p->profile()))},
         {"provider_id", json::Quote(key)},
         {"account_profile", json::Quote(Text(p->account_profile()))},
         {"key_profile", json::Quote(Text(p->key_profile()))}});
    lock.unlock();
    const auto fresh = agent::ListModels(config, stop);
    lock.lock();
    if (!fresh.empty()) {
      std::vector<std::string> rows;
      for (const auto& model : fresh)
        rows.push_back(json::Encode(
            json::Members{{"id", json::Quote(model.id)},
                          {"name", json::Quote(model.name)},
                          {"contextWindowTokens",
                           std::to_string(model.context_window_tokens)}}));
      auto q = db.prepare("INSERT OR REPLACE INTO model_lists VALUES(?,?)");
      q.bind(1, key).bind(2, scrub(json::Encode(rows))).run();
    }
  }
  H::ModelListT result;
  auto stored = db.prepare("SELECT models FROM model_lists WHERE id=?");
  stored.bind(1, key);
  if (stored.step())
    for (const auto& row : json::Array(stored.text(0))) {
      const auto value = json::Object(row);
      auto model = std::make_unique<H::ModelT>();
      model->id = json::Text(value, "id");
      model->name = json::Text(value, "name");
      result.models.push_back(std::move(model));
    }
  return wire::HostReply(request.request_id, std::move(result));
}
}  // namespace tsugumi::host
