#include "host/credentials.hh"

#include <chrono>

#include "agent/provider.hh"
#include "host/failure.hh"

namespace tsugumi::host {
namespace json = ::tsugumi::json;
namespace {
auto Now() -> int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}  // namespace

Credentials::Credentials(StateGate& g, sql::Db& database, std::stop_token token)
    : gate(g), db(database), stop(token) {}

auto Credentials::Import(const std::string& id, const json::Members& value)
    -> void {
  std::lock_guard guard(credential_mutex);
  Save(id, value);
}

auto Credentials::Get(const std::string& id) -> json::Members {
  std::lock_guard lock(gate.mutex);
  auto q = db.prepare("SELECT json FROM credentials WHERE id=?");
  q.bind(1, id);
  if (!q.step())
    throw Failure("not_configured",
                  "Sign in to the selected model account first");
  return json::Object(q.text(0));
}
auto Credentials::Save(const std::string& id, const json::Members& value)
    -> void {
  std::lock_guard lock(gate.mutex);
  auto q = db.prepare("INSERT OR REPLACE INTO credentials VALUES(?,?)");
  q.bind(1, id).bind(2, json::Encode(value)).run();
}
auto Credentials::AccessToken(const std::string& id, std::string_view stale)
    -> std::string {
  std::lock_guard credential_lock(credential_mutex);
  auto value = Get(id);
  if (json::Text(value, "refresh_token").empty()) {
    agent::ProviderError failure("Sign in to ChatGPT again");
    failure.sign_in_required = true;
    throw failure;
  }
  const auto scopes = ' ' + json::Text(value, "scope") + ' ';
  if (scopes.find(" chatgpt.tokens.use.direct ") == std::string::npos)
    throw agent::ProviderError(
        "The ChatGPT plan is not enabled for this registration", false, 0, {},
        "chatgpt_plan_disabled");
  const auto token = json::Text(value, "access_token");
  const auto expires = json::UInt(json::Get(value, "expires_at_ms"));
  // Refresh before expiry so a token remains usable while issuing the request.
  if (!token.empty() &&
      ((!stale.empty() && token != stale) ||
       (stale.empty() && expires > static_cast<uint64_t>(Now() + 300000))))
    return token;
  agent::RefreshedCredential refreshed;
  try {
    refreshed = agent::RefreshCredential(
        {json::Text(value, "token_endpoint"), id,
         json::Text(value, "refresh_token"), json::Text(value, "resource")},
        stop);
  } catch (const agent::ProviderError& error) {
    if (!error.retryable && (error.code == "invalid_grant" ||
                             error.code == "invalid_refresh_token" ||
                             error.code == "token_expired" ||
                             error.code == "refresh_token_expired" ||
                             error.code == "refresh_token_invalidated" ||
                             error.code == "refresh_token_reused" ||
                             error.code == "refresh_token_revoked")) {
      value.erase("access_token");
      value.erase("refresh_token");
      value.erase("expires_at_ms");
      Save(id, value);
      agent::ProviderError failure("Sign in to ChatGPT again", false,
                                   error.status, error.retry_after, error.code,
                                   error.curl_code);
      failure.sign_in_required = true;
      throw failure;
    }
    throw;
  }
  value["access_token"] = json::Quote(refreshed.access_token);
  value["refresh_token"] = json::Quote(refreshed.refresh_token);
  value["expires_at_ms"] =
      std::to_string(Now() + static_cast<int64_t>(refreshed.expires_in) * 1000);
  if (!refreshed.id_token.empty())
    value["id_token"] = json::Quote(refreshed.id_token);
  if (refreshed.has_scope) value["scope"] = json::Quote(refreshed.scope);
  // A successful refresh consumes the old token even if permission narrowed.
  // Persist its replacement before deciding whether inference is allowed.
  Save(id, value);
  const auto granted = ' ' + json::Text(value, "scope") + ' ';
  const bool enabled =
      granted.find(" chatgpt.tokens.use.direct ") != std::string::npos;
  if (!enabled)
    throw agent::ProviderError(
        "The ChatGPT plan is not enabled for this registration", false, 0, {},
        "chatgpt_plan_disabled");
  return refreshed.access_token;
}
auto Credentials::Forget(const std::string& id) -> bool {
  std::lock_guard credential_lock(credential_mutex);
  bool revoked = false;
  try {
    const auto value = Get(id);
    agent::RevokeCredential(json::Text(value, "revocation_endpoint"), id,
                            json::Text(value, "refresh_token"), stop);
    revoked = true;
  } catch (...) {
  }
  std::lock_guard state_lock(gate.mutex);
  auto q = db.prepare("DELETE FROM credentials WHERE id=?");
  q.bind(1, id).run();
  return revoked;
}

auto Credentials::Secrets() -> std::vector<std::string> {
  std::lock_guard lock(gate.mutex);
  std::vector<std::string> secrets;
  auto q = db.prepare("SELECT json FROM credentials");
  while (q.step()) {
    const auto record = json::Object(q.text(0));
    for (const auto* field : {"access_token", "refresh_token", "id_token"}) {
      const auto value = json::Text(record, field);
      if (!value.empty()) secrets.push_back(value);
    }
  }
  return secrets;
}
}  // namespace tsugumi::host
