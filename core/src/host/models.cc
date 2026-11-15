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
  sql::Db model_db((state_dir / "agent.sqlite").string(), true);
  auto q =
      model_db.prepare("SELECT models FROM model_lists WHERE id='chatgpt'");
  if (q.step())
    for (const auto& row : json::Array(q.text(0)))
      models.push_back(json::Text(json::Object(row), "id"));
  return models;
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
      for (const auto& [id, name] : fresh)
        rows.push_back(json::Encode(json::Members{
            {"id", json::Quote(id)}, {"name", json::Quote(name)}}));
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
