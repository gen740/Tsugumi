#include "agent/providers.hh"

#include "host/settings.hh"

namespace tsugumi::agent {
Providers::Providers(host::StateGate& g, sql::Db& database,
                     host::Settings& host_settings)
    : gate(g), db(database), settings(host_settings) {}

auto Providers::Configure(const std::string& id, const std::string& selection)
    -> void {
  auto q = db.prepare("INSERT OR REPLACE INTO providers VALUES(?,?)");
  q.bind(1, id).bind(2, selection).run();
}
auto Providers::Selection(const std::string& id) -> json::Members {
  auto q = db.prepare("SELECT json FROM providers WHERE conversation=?");
  q.bind(1, id);
  if (!q.step()) return {};
  return json::Object(q.text(0));
}
auto Providers::ForConversation(const std::string& id) -> ProviderConfig {
  return settings.Resolve(Selection(id));
}
auto Providers::Profile(const std::string& conversation) -> std::string {
  std::lock_guard lock(gate.mutex);
  auto q = db.prepare("SELECT json FROM providers WHERE conversation=?");
  q.bind(1, conversation);
  const auto profile =
      q.step() ? json::Text(json::Object(q.text(0)), "profile") : "";
  return profile.empty() ? "desktop" : profile;
}
}  // namespace tsugumi::agent
