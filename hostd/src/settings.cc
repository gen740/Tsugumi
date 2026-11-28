#include "host/settings.hh"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/wire.hh"
#include "host/credentials.hh"
#include "host/descriptor.hh"
#include "host/models.hh"
#include "host/skills.hh"
#include "tsugumi/util/c_handle.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/sqlite.hh"

extern const unsigned char tsugumi_agent_settings[];
extern const unsigned long tsugumi_agent_settings_size;

namespace tsugumi::host {
namespace H = Tsugumi::Host;
namespace S = Tsugumi::Service;
namespace j = ::tsugumi::json;
namespace {
auto Text(const flatbuffers::String* p) -> std::string {
  return p ? p->str() : "";
}
auto Now() -> uint64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
auto B64(std::string s) -> std::string {
  auto out = crypto::base64(s);
  std::replace(out.begin(), out.end(), '+', '-');
  std::replace(out.begin(), out.end(), '/', '_');
  while (out.ends_with('=')) out.pop_back();
  return out;
}
auto Un64(std::string s) -> std::string {
  std::replace(s.begin(), s.end(), '-', '+');
  std::replace(s.begin(), s.end(), '_', '/');
  while (s.size() % 4) s += '=';
  std::string out;
  if (!crypto::base64_decode(s, out))
    throw std::runtime_error("Malformed sign-in response");
  return out;
}
auto Escape(const std::string& s) -> std::string {
  auto* c = curl_easy_init();
  auto* raw = curl_easy_escape(c, s.data(), static_cast<int>(s.size()));
  std::string out(raw);
  curl_free(raw);
  curl_easy_cleanup(c);
  return out;
}
auto Form(const j::Members& fields) -> std::string {
  std::string out;
  for (const auto& [k, v] : fields) {
    if (!out.empty()) out += '&';
    out += Escape(k) + '=' + Escape(v);
  }
  return out;
}
auto Query(std::string_view s) -> j::Members {
  j::Members out;
  while (!s.empty()) {
    const auto end = s.find('&');
    const auto part = s.substr(0, end);
    const auto eq = part.find('=');
    auto decode = [](std::string_view v) {
      auto* c = curl_easy_init();
      int n = 0;
      std::string t(v);
      std::replace(t.begin(), t.end(), '+', ' ');
      auto* raw =
          curl_easy_unescape(c, t.data(), static_cast<int>(t.size()), &n);
      std::string result(raw, n);
      curl_free(raw);
      curl_easy_cleanup(c);
      return result;
    };
    const auto key = decode(part.substr(0, eq));
    if (out.contains(key))
      throw std::runtime_error("Duplicate callback parameter");
    out[key] = decode(eq == s.npos ? std::string_view{} : part.substr(eq + 1));
    if (end == s.npos) break;
    s.remove_prefix(end + 1);
  }
  return out;
}
auto HTTP(const std::string& url, const j::Members* form = nullptr,
          std::stop_token stop = {}) -> std::string {
  auto* c = curl_easy_init();
  if (!c) throw std::runtime_error("Sign-in transport unavailable");
  std::string body;
  const auto data = form ? Form(*form) : "";
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(
      c, CURLOPT_XFERINFOFUNCTION,
      +[](void* context, curl_off_t, curl_off_t, curl_off_t,
          curl_off_t) -> int {
        return static_cast<std::stop_token*>(context)->stop_requested();
      });
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, &stop);
  curl_easy_setopt(
      c, CURLOPT_WRITEFUNCTION,
      +[](char* p, size_t n, size_t m, void* context) -> size_t {
        auto& text = *static_cast<std::string*>(context);
        const auto size = n * m;
        if (size > 1024 * 1024 || text.size() > 1024 * 1024 - size) return 0;
        try {
          text.append(p, size);
          return size;
        } catch (...) {
          return 0;
        }
      });
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
  if (form) curl_easy_setopt(c, CURLOPT_POSTFIELDS, data.c_str());
  const auto rc = curl_easy_perform(c);
  long status = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(c);
  // Never forward provider response bodies: they may echo submitted secrets.
  if (rc != CURLE_OK || status != 200)
    throw std::runtime_error("Sign-in HTTP request failed");
  return body;
}
auto URLAllowed(const std::string& url) -> bool {
  auto* parsed = curl_url();
  if (!parsed) return false;
  bool allowed = false;
  if (curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK) {
    char* scheme = nullptr;
    char* host = nullptr;
    char* user = nullptr;
    char* password = nullptr;
    curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0);
    curl_url_get(parsed, CURLUPART_HOST, &host, 0);
    curl_url_get(parsed, CURLUPART_USER, &user, 0);
    curl_url_get(parsed, CURLUPART_PASSWORD, &password, 0);
    allowed = scheme && host && !user && !password &&
              (std::string_view(scheme) == "https" ||
               (std::string_view(scheme) == "http" &&
                std::string_view(host) == "127.0.0.1"));
    curl_free(scheme);
    curl_free(host);
    curl_free(user);
    curl_free(password);
  }
  curl_url_cleanup(parsed);
  return allowed;
}
auto Verify(const std::string& token, const std::string& issuer,
            const std::string& client, const std::string& nonce,
            std::stop_token stop) -> j::Members {
  const auto a = token.find('.'),
             b = token.find('.', a == token.npos ? a : a + 1);
  if (a == token.npos || b == token.npos ||
      token.find('.', b + 1) != token.npos)
    throw std::runtime_error("Malformed ID token");
  const auto header = j::Object(Un64(token.substr(0, a)));
  const auto claims = j::Object(Un64(token.substr(a + 1, b - a - 1)));
  if (j::Text(header, "alg") != "RS256")
    throw std::runtime_error("ID token must use RS256");
  const auto signature = Un64(token.substr(b + 1));
  const auto jwks =
      j::Object(HTTP(issuer + "/.well-known/jwks.json", nullptr, stop));
  bool verified = false;
  for (const auto& raw : j::Array(j::Get(jwks, "keys", "[]"))) {
    const auto key = j::Object(raw);
    if (j::Text(key, "kty") != "RSA" ||
        (!j::Text(header, "kid").empty() &&
         j::Text(key, "kid") != j::Text(header, "kid")) ||
        (!j::Text(key, "alg").empty() && j::Text(key, "alg") != "RS256") ||
        (!j::Text(key, "use").empty() && j::Text(key, "use") != "sig"))
      continue;
    const auto modulus = Un64(j::Text(key, "n")),
               exponent = Un64(j::Text(key, "e"));
    using BnPtr = CHandle<BIGNUM, BN_free>;
    BnPtr n{BN_bin2bn(reinterpret_cast<const unsigned char*>(modulus.data()),
                      modulus.size(), nullptr)};
    BnPtr e{BN_bin2bn(reinterpret_cast<const unsigned char*>(exponent.data()),
                      exponent.size(), nullptr)};
    CHandle<OSSL_PARAM_BLD, OSSL_PARAM_BLD_free> builder{OSSL_PARAM_BLD_new()};
    if (!n || !e || !builder ||
        !OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_N,
                                n.get()) ||
        !OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_E, e.get()))
      continue;
    CHandle<OSSL_PARAM, OSSL_PARAM_free> params{
        OSSL_PARAM_BLD_to_param(builder.get())};
    CHandle<EVP_PKEY_CTX, EVP_PKEY_CTX_free> context{
        EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)};
    if (!params || !context || EVP_PKEY_fromdata_init(context.get()) <= 0)
      continue;
    EVP_PKEY* raw_key = nullptr;
    const auto imported = EVP_PKEY_fromdata(context.get(), &raw_key,
                                            EVP_PKEY_PUBLIC_KEY, params.get());
    CHandle<EVP_PKEY, EVP_PKEY_free> pkey{raw_key};
    CHandle<EVP_MD_CTX, EVP_MD_CTX_free> md{EVP_MD_CTX_new()};
    verified =
        imported > 0 && pkey && md &&
        EVP_DigestVerifyInit(md.get(), nullptr, EVP_sha256(), nullptr,
                             pkey.get()) > 0 &&
        EVP_DigestVerify(
            md.get(), reinterpret_cast<const unsigned char*>(signature.data()),
            signature.size(),
            reinterpret_cast<const unsigned char*>(token.data()), b) == 1;
    if (verified) break;
  }
  // A short skew allowance covers clocks on different sides of the exchange.
  bool audience = j::Text(claims, "aud") == client;
  if (j::Get(claims, "aud").starts_with('['))
    for (const auto& item : j::Array(j::Get(claims, "aud")))
      audience |= j::String(item) == client;
  if (!verified || j::Text(claims, "iss") != issuer || !audience ||
      j::UInt(j::Get(claims, "exp")) * 1000 + 120000 < Now() ||
      j::Text(claims, "nonce") != nonce || j::Text(claims, "sub").empty())
    throw std::runtime_error("ID token verification failed");
  return claims;
}
class SqlSecrets final : public SecretStore {
 public:
  explicit SqlSecrets(sql::Db& db) : db_(db) {}
  auto Get(const std::string& key) -> std::string override {
    auto q = db_.prepare("SELECT value FROM secrets WHERE id=?");
    q.bind(1, key);
    return q.step() ? q.text(0) : "";
  }
  auto Put(const std::string& key, const std::string& value) -> void override {
    auto q = db_.prepare("INSERT OR REPLACE INTO secrets VALUES(?,?)");
    q.bind(1, key).bind(2, value).run();
  }
  auto Erase(const std::string& key) -> void override {
    auto q = db_.prepare("DELETE FROM secrets WHERE id=?");
    q.bind(1, key).run();
  }
  auto Keys(const std::string& prefix) -> std::vector<std::string> override {
    auto q = db_.prepare(
        "SELECT id FROM secrets WHERE substr(id,1,length(?1))=?1 ORDER BY id");
    q.bind(1, prefix);
    std::vector<std::string> keys;
    while (q.step()) keys.push_back(q.text(0));
    return keys;
  }

 private:
  sql::Db& db_;
};
auto Defaults() -> j::Members {
  return {{"agent.provider", j::Quote("anthropic")},
          {"agent.model", j::Quote("claude-sonnet-5-5")},
          {"agent.model.openai", j::Quote("gpt-6.1-sol")},
          {"agent.model.chatgpt", j::Quote("gpt-6.1-sol")},
          {"agent.fixturePort", "8765"},
          {"agent.effort", j::Quote("")},
          {"agent.fastModel", j::Quote("")},
          {"agent.webSearch", "true"},
          {"agent.webSearch.allowedDomains", "[]"},
          {"agent.webSearch.blockedDomains", "[]"},
          {"agent.animateWork", "true"},
          {"agent.allowUnscopedHelpers", "false"},
          {"agent.skillFolders", "[]"},
          {"agent.trustedFolders", "[]"},
          {"vm.cpus", "0"},
          {"vm.memoryGiB", "0"},
          {"vm.storeGiB", "0"},
          {"app.language", j::Quote("ja")}};
}
constexpr std::string_view kCardSecret = "card-secret/";
constexpr char kCachePushTarget[] = "cache.pushTarget";
constexpr char kCachePushUser[] = "cache-push/user";
constexpr char kCachePushSecret[] = "cache-push/secret";

auto ValidateCachePushTarget(const H::CachePushTargetT& target) -> void {
  using Kind = H::CachePushKind;
  const auto& endpoint = target.endpoint;
  const auto& name = target.cache_name;
  if (target.kind == Kind::Own) {
    if (endpoint.empty() && name.empty()) return;
    throw std::runtime_error("The built-in cache needs no endpoint or name");
  }
  if (target.kind == Kind::Attic || target.kind == Kind::Cachix) {
    if (name.empty() || name.size() > 128 || name.front() == '-' ||
        !std::ranges::all_of(name, [](unsigned char c) {
          return c < 128 && (std::isalnum(c) || c == '-' || c == '_');
        }))
      throw std::runtime_error("Invalid cache name");
  } else if (target.kind != Kind::NixCopy || !name.empty()) {
    throw std::runtime_error("Invalid cache push target");
  }
  if (target.kind == Kind::Cachix) {
    if (endpoint.empty()) return;
    throw std::runtime_error("Cachix needs only the cache name");
  }
  if (endpoint.size() > 2048 || endpoint.find_first_of("?#") != endpoint.npos ||
      !std::ranges::all_of(endpoint, [](unsigned char c) {
        return c > 32 && c < 127 && c != '\\' && c != '"';
      }))
    throw std::runtime_error("Invalid cache endpoint");

  const auto url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>(
      curl_url(), curl_url_cleanup);
  if (!url || endpoint.find("://") == endpoint.npos ||
      curl_url_set(url.get(), CURLUPART_URL, endpoint.c_str(),
                   CURLU_NON_SUPPORT_SCHEME | CURLU_DISALLOW_USER))
    throw std::runtime_error("Invalid cache endpoint");
  if (endpoint.starts_with("https://")) return;
  if (target.kind == Kind::NixCopy &&
      (endpoint.starts_with("http://") || endpoint.starts_with("s3://") ||
       endpoint.starts_with("file:///")))
    return;
  throw std::runtime_error("Unsupported cache endpoint scheme");
}

auto ValidateCachePushCredentials(const H::CachePushTargetT& target,
                                  const std::string& user,
                                  const std::string& secret) -> void {
  if (user.empty() && secret.empty()) return;
  using Kind = H::CachePushKind;
  const auto printable = [](const auto& value) {
    return value.size() <= 4096 &&
           std::ranges::all_of(value, [](unsigned char c) {
             return c > 32 && c < 127 && c != '"' && c != '\\' && c != '#';
           });
  };
  if (secret.empty() || !printable(user) || !printable(secret))
    throw std::runtime_error("Invalid cache credentials");
  if ((target.kind == Kind::Attic || target.kind == Kind::Cachix) &&
      user.empty())
    return;
  if (target.kind == Kind::NixCopy && !user.empty() &&
      (target.endpoint.starts_with("https://") ||
       target.endpoint.starts_with("s3://")))
    return;
  throw std::runtime_error("Credentials do not match the cache push target");
}
// One project's Card secrets: card-secret/<project id>/<name>. A project id
// is a short identifier, so it never contains '/'.
auto CardSecretPrefix(const std::string& project) -> std::string {
  if (!crypto::is_short_id(project))
    throw std::runtime_error("Invalid project id");
  return std::string(kCardSecret) + project + "/";
}
// The same rule as a Card's `secrets` entries: [a-z][a-z0-9_]{0,63}.
auto ValidBindingName(std::string_view name) -> bool {
  return !name.empty() && name.size() <= 64 && name[0] >= 'a' &&
         name[0] <= 'z' && std::ranges::all_of(name, [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
         });
}
auto ValidHostPathScope(std::string_view scope) -> bool {
  while (!scope.empty()) {
    if (!scope.starts_with("externals/")) return false;
    scope.remove_prefix(std::string_view("externals/").size());
    const auto end = scope.find('/');
    if (end == std::string_view::npos ||
        !ValidBindingName(scope.substr(0, end)))
      return false;
    scope.remove_prefix(end + 1);
  }
  return true;
}
auto IsSetting(const j::Members& defaults, const std::string& key) -> bool {
  return defaults.contains(key) || key == "agent.drawThoughts" ||
         key == "agent.drawThoughtsAsked" || key == "agent.thoughtModel" ||
         key == "agent.thoughtModel.openai" || key == "agent.speechModel.chatgpt";
}
}  // namespace
struct Settings::State {
  std::filesystem::path directory;
  Credentials& credentials;
  sql::Db& db;
  std::recursive_mutex& mutex;
  std::unique_ptr<SecretStore> secrets;
  struct AttemptResult {
    std::string profile, state = "pending", message, welcome, client;
    bool plan_enabled = false;
  };
  struct Attempt {
    AttemptResult result;
    std::string issuer, resource, client, subject, nonce, verifier,
        oauth_state, redirect;
    bool finished = false;
    std::jthread worker;
  };
  std::map<std::string, std::unique_ptr<Attempt>> active_attempts;
  std::map<std::string, AttemptResult> attempt_results;
  std::map<std::string, std::string> latest;
  Models models;
  std::function<void(std::string)> publish;
  std::condition_variable_any attempt_finished;
  bool stopping = false;
  std::jthread reaper;
  State(const std::filesystem::path& dir, Credentials& credential_store,
        std::unique_ptr<SecretStore> store, Models model_list,
        std::function<void(std::string)> event_sink)
      : directory(dir),
        credentials(credential_store),
        db(credentials.db),
        mutex(credentials.gate.mutex),
        secrets(std::move(store)),
        models(std::move(model_list)),
        publish(std::move(event_sink)) {
    if (!secrets) secrets = std::make_unique<SqlSecrets>(db);
  }
  ~State() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
      for (auto& [_, a] : active_attempts) a->worker.request_stop();
    }
    attempt_finished.notify_one();
    if (reaper.joinable()) reaper.join();
  }
  auto ReapAttempts() -> void {
    std::unique_lock lock(mutex);
    while (!stopping || !active_attempts.empty()) {
      const auto finished = std::ranges::find_if(
          active_attempts, [](const auto& entry) { return entry.second->finished; });
      if (finished == active_attempts.end()) {
        attempt_finished.wait(lock);
        continue;
      }
      auto& attempt = *finished->second;
      const auto profile = attempt.result.profile;
      const bool notify = attempt.result.state != "cancelled";
      attempt_results.emplace(finished->first, std::move(attempt.result));
      auto worker = std::move(attempt.worker);
      active_attempts.erase(finished);  // Releases the authorization material.
      lock.unlock();
      worker.join();
      if (notify) Publish<H::SignInChangedT>(profile);
      lock.lock();
    }
  }
  auto FindAttemptResult(const std::string& id) -> const AttemptResult* {
    if (const auto it = active_attempts.find(id); it != active_attempts.end())
      return &it->second->result;
    if (const auto it = attempt_results.find(id); it != attempt_results.end())
      return &it->second;
    return nullptr;
  }
  auto Revision() -> std::string {
    auto query = db.prepare(
        "SELECT store || ':' || printf('%019lld',revision) FROM "
        "settings_revision");
    query.step();
    return query.text(0);
  }
  auto CachePushTarget() -> H::CachePushTargetT {
    H::CachePushTargetT target;
    auto query = db.prepare(
        "SELECT value FROM setting_values WHERE scope='' AND key=?");
    query.bind(1, kCachePushTarget);
    if (query.step()) {
      const auto config = j::Object(query.text(0));
      target.kind =
          static_cast<H::CachePushKind>(j::UInt(j::Get(config, "kind")));
      target.endpoint = j::Text(config, "endpoint");
      target.cache_name = j::Text(config, "cacheName");
    }
    target.has_credentials = !secrets->Get(kCachePushSecret).empty();
    return target;
  }
  auto SetCachePushTarget(const H::CachePushTargetPutRequest& input)
      -> H::CachePushTargetT {
    H::CachePushTargetT target;
    target.kind = input.kind();
    target.endpoint = Text(input.endpoint());
    target.cache_name = Text(input.cache_name());
    ValidateCachePushTarget(target);
    auto user = Text(input.credential_user());
    auto secret = Text(input.credential_secret());
    ValidateCachePushCredentials(target, user, secret);
    if (input.clear_credentials() && !secret.empty())
      throw std::runtime_error("Choose whether to replace or clear credentials");
    const auto previous = CachePushTarget();
    const bool same = previous.kind == target.kind &&
                      previous.endpoint == target.endpoint &&
                      previous.cache_name == target.cache_name;
    sql::Transaction transaction(db);
    if (!secret.empty()) {
      if (user.empty())
        secrets->Erase(kCachePushUser);
      else
        secrets->Put(kCachePushUser, user);
      secrets->Put(kCachePushSecret, secret);
    } else if (!same || input.clear_credentials()) {
      secrets->Erase(kCachePushUser);
      secrets->Erase(kCachePushSecret);
    }
    db.prepare("INSERT OR REPLACE INTO setting_values VALUES('',?,?)")
        .bind(1, kCachePushTarget)
        .bind(2, j::Encode(j::Members{
                     {"kind",
                      std::to_string(static_cast<unsigned>(target.kind))},
                     {"endpoint", j::Quote(target.endpoint)},
                     {"cacheName", j::Quote(target.cache_name)}}))
        .run();
    transaction.commit();
    target.has_credentials =
        !secret.empty() || (same && !input.clear_credentials() &&
                            previous.has_credentials);
    return target;
  }
  auto Values(const std::string& surface = {}) -> j::Members {
    auto values = Defaults();
    auto query = db.prepare(
        "SELECT key,value FROM setting_values WHERE scope IN ('',?)");
    query.bind(1, surface);
    const auto defaults = Defaults();
    while (query.step())
      if (IsSetting(defaults, query.text(0)))
        values[query.text(0)] = query.text(1);
    return values;
  }
  auto Get(const std::string& surface) -> H::SettingsT {
    auto values = Values(surface);
    std::vector<std::string> roots;
    for (const auto& path : host::TrustedFolders(db))
      roots.push_back(j::Quote(path));
    values["agent.trustedFolders"] = j::Encode(roots);
    H::SettingsT out;
    out.revision = Revision();
    for (const auto& [k, v] : values) {
      auto item = std::make_unique<H::SettingT>();
      item->key = k;
      item->value_json = v;
      out.values.push_back(std::move(item));
    }
    return out;
  }
  auto Account(const std::string& profile, const std::string& id)
      -> j::Members {
    auto q = db.prepare("SELECT json FROM accounts WHERE profile=? AND id=?");
    q.bind(1, profile).bind(2, id);
    if (!q.step()) throw std::runtime_error("Unknown ChatGPT account");
    return j::Object(q.text(0));
  }
  template <typename Event>
  auto Publish(const std::string& profile) -> void {
    if (!publish) return;
    Event event;
    event.profile = profile;
    publish(agent::wire::HostEvent(std::move(event)));
  }
  auto SaveAccount(const std::string& profile, const std::string& id,
                   const j::Members& value) -> void {
    const auto encoded = j::Encode(value);
    auto old = db.prepare("SELECT json FROM accounts WHERE profile=? AND id=?");
    old.bind(1, profile).bind(2, id);
    if (old.step() && old.text(0) == encoded) return;
    auto q = db.prepare("INSERT OR REPLACE INTO accounts VALUES(?,?,?)");
    q.bind(1, profile).bind(2, id).bind(3, encoded).run();
    Publish<H::ProviderChangedT>(profile);
  }
  auto ProviderStatus(const std::string& profile, const std::string& id)
      -> H::ProviderStatusT {
    H::ProviderStatusT out;
    out.id = id;
    if (id == "chatgpt") {
      auto q = db.prepare("SELECT json FROM accounts WHERE profile=?");
      q.bind(1, profile);
      while (q.step()) {
        const auto v = j::Object(q.text(0));
        if (j::Bool(j::Get(v, "active"))) {
          if (!j::Bool(j::Get(v, "signed_in"))) {
            out.code = "chatgpt_sign_in_required";
            out.message = "Sign in to ChatGPT again";
          } else if (!j::Bool(j::Get(v, "plan_enabled"))) {
            out.code = "chatgpt_plan_disabled";
            out.message =
                "The ChatGPT plan is not enabled for this registration";
          }
        }
        out.configured |= j::Bool(j::Get(v, "active")) &&
                          j::Bool(j::Get(v, "signed_in")) &&
                          j::Bool(j::Get(v, "plan_enabled"));
      }
      if (out.configured) {
        out.source = "oauth";
        out.code.clear();
        out.message.clear();
      } else if (out.message.empty()) {
        out.message = "Sign in to ChatGPT first";
      }
      return out;
    }
    out.configured = !secrets->Get(profile + ":" + id).empty();
    if (out.configured) out.source = "hostd";
    if (id == "fixture") out.configured = true;
    if (!out.configured) out.message = "No API key configured";
    return out;
  }
  auto CardSecretList(const std::string& project) -> H::CardSecretListT {
    const auto prefix = CardSecretPrefix(project);
    H::CardSecretListT out;
    for (const auto& key : secrets->Keys(prefix))
      out.names.push_back(key.substr(prefix.size()));
    return out;
  }
  auto Status(const std::string& profile, const std::string& attempt)
      -> H::SignInStatusT {
    H::SignInStatusT out;
    out.state = "idle";
    const auto selected = attempt.empty() && latest.contains(profile)
                              ? latest.at(profile)
                              : attempt;
    if (!selected.empty()) {
      const auto* a = FindAttemptResult(selected);
      if (!a || a->profile != profile) {
        out.state = "failed";
        out.message = "Sign-in was interrupted; try again";
      } else {
        out.attempt_id = selected;
        out.state = a->state;
        out.message = a->message;
        out.welcome = a->welcome;
        if (a->state == "complete") {
          out.client_id = a->client;
          out.plan_enabled = a->plan_enabled;
        }
      }
    }
    auto q = db.prepare(
        "SELECT id,json FROM accounts WHERE profile=? ORDER BY rowid");
    q.bind(1, profile);
    while (q.step()) {
      const auto v = j::Object(q.text(1));
      auto a = std::make_unique<H::AccountT>();
      a->client_id = q.text(0);
      a->email = j::Text(v, "email");
      a->signed_in = j::Bool(j::Get(v, "signed_in"));
      a->plan_enabled = j::Bool(j::Get(v, "plan_enabled"));
      a->active = j::Bool(j::Get(v, "active"));
      out.accounts.push_back(std::move(a));
    }
    return out;
  }
  auto Choose(const std::string& profile, const std::string& id) -> void {
    auto q = db.prepare("SELECT id,json FROM accounts WHERE profile=?");
    q.bind(1, profile);
    std::vector<std::pair<std::string, j::Members>> rows;
    while (q.step()) {
      auto v = j::Object(q.text(1));
      v["active"] = q.text(0) == id ? "true" : "false";
      rows.emplace_back(q.text(0), v);
    }
    for (const auto& [key, v] : rows) SaveAccount(profile, key, v);
  }
  auto Cancel(Attempt& attempt) -> void {
    if (attempt.result.state != "pending") return;
    attempt.worker.request_stop();
    attempt.result.state = "cancelled";
  }
  auto Begin(const H::SignInBeginRequest& p) -> H::SignInBeginT {
    auto attempt = std::make_unique<Attempt>();
    auto* a = attempt.get();
    a->result.profile = p.profile()->str();
    const auto& profile = a->result.profile;
    Attempt* previous_attempt = nullptr;
    if (const auto current = latest.find(profile); current != latest.end()) {
      if (const auto previous = active_attempts.find(current->second);
          previous != active_attempts.end()) {
        previous_attempt = previous->second.get();
        const auto& state = previous_attempt->result.state;
        if (state == "committing" || (state == "pending" && !p.replace_pending()))
          throw std::runtime_error("Sign-in is already in progress");
      }
    }
    const auto id = crypto::uuid4();
    a->client = Text(p.client_id());
    a->issuer = Text(p.issuer());
    if (a->issuer.empty()) a->issuer = "https://auth.openai.com";
    while (a->issuer.ends_with('/')) a->issuer.pop_back();
    a->resource = Text(p.resource());
    if (a->resource.empty()) a->resource = "https://api.openai.com/v1";
    if (!URLAllowed(a->issuer) || !URLAllowed(a->resource))
      throw std::runtime_error("Sign-in requires HTTPS or loopback");
    j::Members previous;
    if (!a->client.empty()) {
      previous = Account(profile, a->client);
      if (j::Text(previous, "issuer") != a->issuer)
        throw std::runtime_error("Account issuer mismatch");
      a->subject = j::Text(previous, "subject");
    }
#ifdef __linux__
    Descriptor listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
#else
    Descriptor listener(::socket(AF_INET, SOCK_STREAM, 0));
#endif
    if (listener.get() < 0)
      throw std::runtime_error("Cannot open sign-in callback");
#ifndef __linux__
    if (::fcntl(listener.get(), F_SETFD, FD_CLOEXEC) < 0)
      throw std::runtime_error("Cannot protect sign-in callback across exec");
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&address),
               sizeof address) != 0 ||
        ::listen(listener.get(), 4) != 0)
      throw std::runtime_error("Cannot bind sign-in callback");
    socklen_t length = sizeof address;
    ::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length);
    a->redirect =
        "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) +
        "/auth/callback";
    a->oauth_state = crypto::random_token(32);
    a->nonce = crypto::random_token(32);
    a->verifier = crypto::random_token(32);
    auto host_query = db.prepare("SELECT id FROM oauth_hosts WHERE profile=?");
    host_query.bind(1, profile);
    auto hostid = host_query.step() ? host_query.text(0) : "";
    if (hostid.empty()) {
      hostid = "urn:uuid:" + crypto::uuid4();
      auto q = db.prepare("INSERT INTO oauth_hosts VALUES(?,?)");
      q.bind(1, profile).bind(2, hostid).run();
    }
    const auto hash = crypto::sha256_hex(a->verifier);
    std::string binary;
    for (size_t i = 0; i < hash.size(); i += 2)
      binary += static_cast<char>(std::stoul(hash.substr(i, 2), nullptr, 16));
    j::Members query{
        {"client_id", a->client.empty() ? "dynamic_agent_client" : a->client},
        {"ext_agent_host_id", hostid},
        {"response_type", "code"},
        {"redirect_uri", a->redirect},
        {"scope",
         "openid profile email offline_access resource.invoke "
         "chatgpt.tokens.use.direct"},
        {"resource", a->resource},
        {"state", a->oauth_state},
        {"nonce", a->nonce},
        {"code_challenge_method", "S256"},
        {"code_challenge", B64(binary)}};
    if (!previous.empty() && !j::Text(previous, "email").empty())
      query["login_hint"] = j::Text(previous, "email");
    else
      query["agent_name_hint"] = "Tsugumi";
    if (p.consent()) query["prompt"] = "consent";
    H::SignInBeginT out;
    out.attempt_id = id;
    out.url = a->issuer + "/api/accounts/authorize?" + Form(query);
    if (!reaper.joinable()) reaper = std::jthread([this] { ReapAttempts(); });
    active_attempts.emplace(id, std::move(attempt));
    auto run = [this, a, listener = std::move(listener)](
                   std::stop_token stop) mutable {
      try {
        j::Members cb;
        struct CallbackPipe {
          int fds[2] = {-1, -1};
          ~CallbackPipe() {
            for (auto fd : fds)
              if (fd >= 0) ::close(fd);
          }
        } wake;
        if (::pipe(wake.fds) != 0)
          throw std::runtime_error("Cannot open sign-in cancellation pipe");
        for (auto fd : wake.fds) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        std::stop_callback cancelled(
            stop, [&] { (void)::write(wake.fds[1], "x", 1); });
        // A browser may open a connection it never uses (a preconnect): read
        // every connection as its bytes arrive, so none holds up the one that
        // carries the callback.
        struct Peer {
          int fd;
          std::string input;
        };
        struct Peers : std::vector<Peer> {
          ~Peers() {
            for (auto& peer : *this) ::close(peer.fd);
          }
        } peers;
        bool done = false;
        while (!done && !stop.stop_requested()) {
          std::vector<pollfd> fds{{listener.get(), POLLIN, 0},
                                  {wake.fds[0], POLLIN, 0}};
          for (const auto& peer : peers) fds.push_back({peer.fd, POLLIN, 0});
          if (::poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR) continue;
            break;
          }
          if (stop.stop_requested() || fds[1].revents) break;
          if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
          for (size_t i = peers.size(); i-- > 0;) {
            if (!fds[i + 2].revents) continue;
            auto& peer = peers[i];
            char buffer[2048];
            const auto n = ::recv(peer.fd, buffer, sizeof buffer, 0);
            if (n > 0) peer.input.append(buffer, n);
            const bool complete =
                peer.input.find("\r\n\r\n") != peer.input.npos;
            // Bound the memory a loopback peer can claim for callback headers.
            if (!complete && n > 0 && peer.input.size() < 16384) continue;
            bool valid = false;
            try {
              const auto end = peer.input.find(" HTTP/");
              if (complete && peer.input.size() < 16384 &&
                  peer.input.starts_with("GET /auth/callback?") &&
                  end != peer.input.npos) {
                auto query =
                    Query(std::string_view(peer.input).substr(19, end - 19));
                valid = query["state"] == a->oauth_state;
                if (valid) cb = std::move(query);
              }
            } catch (...) {
            }
            const std::string response =
                valid
                    ? "HTTP/1.1 200 OK\r\nContent-Type: text/html; "
                      "charset=utf-8\r\nCache-Control: "
                      "no-store\r\nReferrer-Policy: no-referrer\r\nConnection: "
                      "close\r\n\r\n<!doctype "
                      "html><title>Tsugumi</title>サインインの結果は Tsugumi "
                      "で確認できます。このタブを閉じて戻ってください。 / "
                      "Check the result in Tsugumi. Close this tab and return."
                    : "HTTP/1.1 404 Not Found\r\nConnection: "
                      "close\r\nContent-Length: 0\r\n\r\n";
            if (complete || n > 0)
              ::send(peer.fd, response.data(), response.size(), 0);
            ::close(peer.fd);
            peers.erase(peers.begin() + static_cast<ptrdiff_t>(i));
            if (valid) done = true;
          }
          if (!done && (fds[0].revents & POLLIN)) {
            const int fd = ::accept(listener.get(), nullptr, nullptr);
            if (fd >= 0) {
              ::fcntl(fd, F_SETFD, FD_CLOEXEC);
              peers.push_back({fd, {}});
            } else if (errno != EINTR && errno != ECONNABORTED) {
              break;
            }
          }
        }
        listener.reset();
        if (stop.stop_requested())
          throw std::runtime_error("Sign-in cancelled");
        if (cb.empty()) throw std::runtime_error("Sign-in callback closed");
        if (cb.contains("error"))
          throw std::runtime_error("Sign-in was declined");
        if (a->client.empty()) {
          a->client = cb["client_id"];
          if (a->client.empty() || a->client == "dynamic_agent_client")
            throw std::runtime_error("Registration returned no client ID");
        } else if (cb.contains("client_id") && cb["client_id"] != a->client)
          throw std::runtime_error("Registration mismatch");
        if (cb["code"].empty())
          throw std::runtime_error("Sign-in returned no code");
        const j::Members form{{"grant_type", "authorization_code"},
                              {"client_id", a->client},
                              {"code", cb["code"]},
                              {"code_verifier", a->verifier},
                              {"redirect_uri", a->redirect},
                              {"resource", a->resource}};
        const auto t = j::Object(
            HTTP(a->issuer + "/api/accounts/oauth/token", &form, stop));
        for (const auto* key : {"access_token", "refresh_token", "id_token"})
          if (j::Text(t, key).empty())
            throw std::runtime_error("Sign-in response lacks tokens");
        const auto claims = Verify(j::Text(t, "id_token"), a->issuer, a->client,
                                   a->nonce, stop);
        if (!a->subject.empty() && a->subject != j::Text(claims, "sub"))
          throw std::runtime_error("Signed into another account");
        const auto scope = j::Text(t, "scope", cb["scope"]);
        const bool plan =
            (" " + scope + " ").find(" chatgpt.tokens.use.direct ") !=
            std::string::npos;
        j::Members credential = t;
        credential["scope"] = j::Quote(scope);
        credential["token_endpoint"] =
            j::Quote(a->issuer + "/api/accounts/oauth/token");
        credential["revocation_endpoint"] =
            j::Quote(a->issuer + "/api/accounts/oauth/revoke");
        credential["resource"] = j::Quote(a->resource);
        // The issuer may omit expires_in; preserve the documented token
        // lifetime.
        credential["expires_at_ms"] = std::to_string(
            Now() + j::UInt(j::Get(t, "expires_in"), 3600) * 1000);
        std::lock_guard operation(credentials.credential_mutex);
        std::lock_guard guard(mutex);
        if (stop.stop_requested())
          throw std::runtime_error("Sign-in cancelled");
        sql::Transaction tx(db);
        credentials.Save(a->client, credential);
        bool welcomed = false;
        try {
          welcomed = j::Bool(
              j::Get(Account(a->result.profile, a->client), "welcomed"));
        } catch (...) {
        }
        SaveAccount(a->result.profile, a->client,
                    {{"issuer", j::Quote(a->issuer)},
                     {"resource", j::Quote(a->resource)},
                     {"subject", j::Get(claims, "sub")},
                     {"email", j::Get(claims, "email", j::Quote(""))},
                     {"signed_in", "true"},
                     {"plan_enabled", plan ? "true" : "false"},
                     {"welcomed", welcomed ? "true" : "false"}});
        Choose(a->result.profile, a->client);
        tx.commit();
        a->result.state = "complete";
        a->result.client = a->client;
        a->result.plan_enabled = plan;
        if (plan && !welcomed) a->result.welcome = a->client;
      } catch (...) {
        listener.reset();
        std::lock_guard guard(mutex);
        if (a->result.state != "cancelled") {
          a->result.state = "failed";
          a->result.message = "Sign-in failed; try again";
        }
      }
      std::lock_guard guard(mutex);
      a->finished = true;
      attempt_finished.notify_one();
    };
    try {
      a->worker = std::jthread(std::move(run));
    } catch (...) {
      active_attempts.erase(id);
      throw;
    }
    latest[profile] = id;
    if (previous_attempt) Cancel(*previous_attempt);
    Publish<H::SignInChangedT>(profile);
    return out;
  }
};
Settings::Settings(const std::filesystem::path& d, Credentials& credentials,
                   std::unique_ptr<SecretStore> store, Models models,
                   std::function<void(std::string)> publish)
    : state_(std::make_unique<State>(
          d, credentials, std::move(store), std::move(models),
          std::move(publish))) {}
Settings::~Settings() = default;
auto Settings::Handles(H::RequestPayload type) -> bool {
  switch (type) {
    case H::RequestPayload::ModelSelectionRequest:
    case H::RequestPayload::SettingsGetRequest:
    case H::RequestPayload::SettingsUpdateRequest:
    case H::RequestPayload::CachePushTargetGetRequest:
    case H::RequestPayload::CachePushTargetPutRequest:
    case H::RequestPayload::ProviderSetRequest:
    case H::RequestPayload::ProviderStatusRequest:
    case H::RequestPayload::SignInBeginRequest:
    case H::RequestPayload::SignInActionRequest:
    case H::RequestPayload::SignInStatusRequest:
    case H::RequestPayload::HostPathListRequest:
    case H::RequestPayload::HostPathSetRequest:
    case H::RequestPayload::CardSecretListRequest:
    case H::RequestPayload::CardSecretSetRequest:
      return true;
    default:
      return false;
  }
}
auto Settings::Handle(const H::Request& request, uint64_t id) -> std::string {
  auto& s = *state_;
  std::unique_lock lock(s.mutex);
  try {
    switch (request.value_type()) {
      case H::RequestPayload::ModelSelectionRequest: {
        const auto& p = *request.value_as_ModelSelectionRequest();
        lock.unlock();
        H::ModelSelectionT result;
        result.provider = std::make_unique<S::ProviderSelectionT>(
            ModelSelection(p.profile()->str(), p.purpose()->str()));
        return agent::wire::HostReply(id, std::move(result));
      }
      case H::RequestPayload::SettingsGetRequest:
        return agent::wire::HostReply(
            id, s.Get(request.value_as_SettingsGetRequest()->profile()->str()));
      case H::RequestPayload::CachePushTargetGetRequest:
        return agent::wire::HostReply(id, s.CachePushTarget());
      case H::RequestPayload::CachePushTargetPutRequest:
        return agent::wire::HostReply(
            id,
            s.SetCachePushTarget(*request.value_as_CachePushTargetPutRequest()));
      case H::RequestPayload::SettingsUpdateRequest: {
        const auto& p = *request.value_as_SettingsUpdateRequest();
        sql::Transaction tx(s.db);
        const auto surface = p.profile()->str();
        if (surface.empty()) throw std::runtime_error("Missing settings surface");
        if (s.Revision() != p.revision()->string_view()) {
          S::FailureT f;
          f.code = "conflict";
          f.message = "Settings changed; read the latest revision";
          return agent::wire::HostReply(id, std::move(f));
        }
        if (const auto* expected = p.chatgpt_account()) {
          const auto account = s.Account(expected->profile()->str(),
                                         expected->client_id()->str());
          if (!j::Bool(j::Get(account, "active")) ||
              !j::Bool(j::Get(account, "signed_in")) ||
              !j::Bool(j::Get(account, "plan_enabled"))) {
            S::FailureT f;
            f.code = "chatgpt_account_changed";
            f.message =
                "The ChatGPT account has changed. Sign in again to finish "
                "setup.";
            return agent::wire::HostReply(id, std::move(f));
          }
        }
        const auto defaults = Defaults();
        auto values = s.Values(surface);
        if (p.changes())
          for (const auto* item : *p.changes()) {
            const auto key = item->key()->str();
            auto value = item->value_json()->str();
            if (value.size() > 65536 || key == "agent.oauthHostID" ||
                !(key.starts_with("agent.") || key.starts_with("vm.") ||
                  key == "app.language"))
              throw std::runtime_error("Invalid setting");
            j::Object("{\"value\":" + value + "}");
            if (!IsSetting(defaults, key))
              throw std::runtime_error("Unknown setting");
            if (key == "app.language" && j::String(value) != "ja" &&
                j::String(value) != "en")
              throw std::runtime_error("Unknown language");
            if (key == "agent.trustedFolders") {
              std::vector<std::string> folders;
              for (const auto& raw : j::Array(value))
                folders.push_back(j::String(raw));
              host::SetTrustedFolders(s.db, folders);
              continue;
            }
            if (key.starts_with("vm.")) {
              const auto limits = VMResourceLimits();
              const auto [low, high] =
                  key == "vm.cpus"        ? std::pair{1u, limits.cpus}
                  : key == "vm.memoryGiB" ? std::pair{2u, limits.memory_gib}
                                          : std::pair{16u, limits.store_gib};
              const auto n = j::UInt(value, UINT64_MAX);
              if (n != 0 && (n < low || n > high))
                throw std::runtime_error("VM size out of range");
              value = std::to_string(n);
            }
            if (key == "agent.skillFolders")
              for (const auto& folder : j::Array(value)) {
                const auto entry = j::Object(folder);
                if (j::Text(entry, "path").empty() ||
                    (j::Get(entry, "enabled") != "true" &&
                     j::Get(entry, "enabled") != "false"))
                  throw std::runtime_error("Invalid skill folder");
              }
            if (key == "agent.webSearch.allowedDomains" ||
                key == "agent.webSearch.blockedDomains") {
              std::vector<std::string> domains;
              for (const auto& raw : j::Array(value)) {
                auto domain = j::String(raw);
                std::ranges::transform(domain, domain.begin(), [](char c) {
                  return static_cast<char>(
                      std::tolower(static_cast<unsigned char>(c)));
                });
                if (domain.empty() ||
                    domain.find_first_of(" \t\r\n/:") != domain.npos)
                  throw std::runtime_error("Invalid web search domain");
                domains.push_back(j::Quote(domain));
              }
              value = j::Encode(domains);
            }
            if ((key == "agent.webSearch" || key == "agent.drawThoughts" ||
                 key == "agent.drawThoughtsAsked" ||
                 key == "agent.animateWork" ||
                 key == "agent.allowUnscopedHelpers") &&
                value != "true" && value != "false")
              throw std::runtime_error("Expected a boolean setting");
            if ((key == "agent.provider" || key.starts_with("agent.model") ||
                 key == "agent.effort" || key == "agent.fastModel") &&
                (value.empty() || value.front() != '"'))
              throw std::runtime_error("Expected a text setting");

            if (key == "agent.provider" && j::String(value) != "anthropic" &&
                j::String(value) != "openai" && j::String(value) != "chatgpt" &&
                j::String(value) != "fixture")
              throw std::runtime_error("Unknown provider");
            if (key == "agent.fixturePort" &&
                (j::UInt(value) < 1 || j::UInt(value) > 65535))
              throw std::runtime_error("Invalid fixture port");
            if (key.find("secret") != key.npos ||
                key.find("token") != key.npos ||
                key.find("credential") != key.npos ||
                key.find("key") != key.npos)
              throw std::runtime_error("Secrets are not settings");
            auto write = s.db.prepare(
                "INSERT OR REPLACE INTO setting_values VALUES(?,?,?)");
            write.bind(1, key == "agent.skillFolders" ? surface : "")
                .bind(2, key)
                .bind(3, value)
                .run();
            values[key] = value;
          }
        s.db.exec("UPDATE settings_revision SET revision=revision+1");
        tx.commit();
        return agent::wire::HostReply(id, s.Get(surface));
      }
      case H::RequestPayload::ProviderSetRequest: {
        const auto& p = *request.value_as_ProviderSetRequest();
        const auto profile = p.profile()->str(), provider = p.id()->str(),
                   key = profile + ":" + provider;
        if (p.provider() && p.provider()->api_key()) {
          const auto secret = p.provider()->api_key()->str();
          if (secret.empty() || secret.find_first_of("\r\n") != secret.npos ||
              secret.find('\0') != secret.npos)
            throw std::runtime_error("Invalid API key");
        }
        if (provider != "anthropic" && provider != "openai" &&
            provider != "fixture")
          throw std::runtime_error("Unknown keyed provider");
        const auto before = s.ProviderStatus(profile, provider);
        if (p.remove()) {
          s.secrets->Erase(key);
          auto q =
              s.db.prepare("DELETE FROM provider_connections WHERE profile=? AND id=?");
          q.bind(1, profile).bind(2, provider).run();
        } else if (p.provider()) {
          auto value = j::Object(agent::wire::ProviderJson(*p.provider()));
          const auto endpoint = j::Text(value, "baseURL");
          if (endpoint.empty() && j::Text(value, "model").empty()) {
            auto old = s.db.prepare(
                "SELECT json FROM provider_connections WHERE profile=? AND id=?");
            old.bind(1, profile).bind(2, provider);
            value = old.step() ? j::Object(old.text(0)) : j::Members{};
          }
          if (!endpoint.empty() && !URLAllowed(endpoint))
            throw std::runtime_error("Provider requires HTTPS or loopback");
          const auto secret = Text(p.provider()->api_key());
          if (!secret.empty()) s.secrets->Put(key, secret);
          value.erase("credential");
          value.erase("apiKey");
          value.erase("credential_id");
          auto q =
              s.db.prepare("INSERT OR REPLACE INTO provider_connections VALUES(?,?,?)");
          q.bind(1, profile).bind(2, provider).bind(3, j::Encode(value)).run();
        }
        const auto after = s.ProviderStatus(profile, provider);
        if (before.configured != after.configured ||
            before.message != after.message ||
            before.endpoint != after.endpoint || before.source != after.source)
          s.Publish<H::ProviderChangedT>(profile);
        return agent::wire::HostReply(id, after);
      }
      case H::RequestPayload::HostPathListRequest:
        return agent::wire::HostReply(
            id,
            HostPaths(
                request.value_as_HostPathListRequest()->project_id()->str()));
      case H::RequestPayload::HostPathSetRequest: {
        const auto& p = *request.value_as_HostPathSetRequest();
        const auto project = p.project_id()->str();
        const auto scope = Text(p.scope());
        const auto name = p.name()->str();
        if (!crypto::is_short_id(project))
          throw std::runtime_error("Invalid project id");
        if (!ValidHostPathScope(scope) || !ValidBindingName(name))
          throw std::runtime_error("Invalid host path scope or name");
        if (p.remove()) {
          s.db.prepare(
                  "DELETE FROM host_path_bindings "
                  "WHERE project=? AND scope=? AND name=?")
              .bind(1, project)
              .bind(2, scope)
              .bind(3, name)
              .run();
        } else {
          const auto input = Text(p.path());
          if (input.find('\0') != std::string::npos ||
              !std::filesystem::path(input).is_absolute())
            throw std::runtime_error("Choose an absolute folder on this host");
          std::error_code ec;
          const auto path = std::filesystem::canonical(input, ec);
          if (ec || !std::filesystem::is_directory(path, ec))
            throw std::runtime_error("The selected host folder is unavailable");
          s.db.prepare(
                  "INSERT INTO host_path_bindings VALUES(?,?,?,?) "
                  "ON CONFLICT(project,scope,name) DO UPDATE SET "
                  "path=excluded.path")
              .bind(1, project)
              .bind(2, scope)
              .bind(3, name)
              .bind(4, path.string())
              .run();
        }
        return agent::wire::HostReply(id, HostPaths(project));
      }
      case H::RequestPayload::CardSecretSetRequest: {
        const auto& p = *request.value_as_CardSecretSetRequest();
        const auto project = p.project_id()->str(), name = p.name()->str();
        const auto prefix = CardSecretPrefix(project);
        if (!ValidBindingName(name))
          throw std::runtime_error("Invalid Card secret name");
        if (p.remove()) {
          s.secrets->Erase(prefix + name);
        } else {
          const auto value = Text(p.value());
          if (value.empty() || value.size() > 65536 ||
              value.find('\0') != value.npos)
            throw std::runtime_error("Invalid Card secret value");
          s.secrets->Put(prefix + name, value);
        }
        return agent::wire::HostReply(id, s.CardSecretList(project));
      }
      case H::RequestPayload::CardSecretListRequest:
        return agent::wire::HostReply(
            id,
            s.CardSecretList(
                request.value_as_CardSecretListRequest()->project_id()->str()));
      case H::RequestPayload::ProviderStatusRequest: {
        const auto& p = *request.value_as_ProviderStatusRequest();
        return agent::wire::HostReply(
            id, s.ProviderStatus(p.profile()->str(), p.id()->str()));
      }
      case H::RequestPayload::SignInBeginRequest:
        return agent::wire::HostReply(
            id, s.Begin(*request.value_as_SignInBeginRequest()));
      case H::RequestPayload::SignInStatusRequest: {
        const auto& p = *request.value_as_SignInStatusRequest();
        return agent::wire::HostReply(
            id, s.Status(p.profile()->str(), Text(p.attempt_id())));
      }
      case H::RequestPayload::SignInActionRequest: {
        const auto& p = *request.value_as_SignInActionRequest();
        const auto profile = p.profile()->str(), client = Text(p.client_id()),
                   action = p.action()->str(), attempt = Text(p.attempt_id());
        bool revoked = false;
        if (action == "cancel") {
          const auto* result = s.FindAttemptResult(attempt);
          if (!result || result->profile != profile)
            throw std::runtime_error("Unknown sign-in attempt");
          if (const auto it = s.active_attempts.find(attempt);
              it != s.active_attempts.end())
            s.Cancel(*it->second);
        } else {
          auto value = s.Account(profile, client);
          if (action == "select")
            s.Choose(profile, client);
          else if (action == "welcomed") {
            value["welcomed"] = "true";
            s.SaveAccount(profile, client, value);
            const auto clear_welcome = [&](State::AttemptResult& result) {
              if (result.profile == profile && result.welcome == client)
                result.welcome.clear();
            };
            for (auto& [_, active] : s.active_attempts)
              clear_welcome(active->result);
            for (auto& [_, result] : s.attempt_results) clear_welcome(result);
          } else if (action == "signout") {
            for (auto& [_, active] : s.active_attempts)
              if (active->result.profile == profile) s.Cancel(*active);
            lock.unlock();
            std::lock_guard operation(s.credentials.credential_mutex);
            revoked = s.credentials.Revoke(client);
            lock.lock();
            // A registration commit may have completed while we waited.
            value = s.Account(profile, client);
            sql::Transaction tx(s.db);
            auto remove = s.db.prepare("DELETE FROM credentials WHERE id=?");
            remove.bind(1, client).run();
            value["signed_in"] = "false";
            s.SaveAccount(profile, client, value);
            tx.commit();
          } else
            throw std::runtime_error("Unknown sign-in action");
        }
        s.Publish<H::SignInChangedT>(profile);
        auto out = s.Status(profile, attempt);
        out.revoked = revoked;
        return agent::wire::HostReply(id, std::move(out));
      }
      default:
        throw std::runtime_error("Unknown settings request");
    }
  } catch (...) {
    S::FailureT f;
    f.code = "invalid_request";
    f.message = "Host settings operation failed";
    return agent::wire::HostReply(id, std::move(f));
  }
}
auto Settings::Redact(std::string text) -> std::string {
  return Redact(std::move(text), RedactionStrings());
}
auto Settings::RedactionStrings() -> std::vector<std::string> {
  auto& s = *state_;
  std::lock_guard lock(s.mutex);
  std::vector<std::string> secrets;
  auto rows = s.db.prepare("SELECT profile,id FROM provider_connections");
  while (rows.step()) {
    auto secret = s.secrets->Get(rows.text(0) + ":" + rows.text(1));
    if (secret.empty()) continue;
    const auto encoded = j::Quote(secret);
    auto escaped = encoded.substr(1, encoded.size() - 2);
    const bool differs = escaped != secret;
    secrets.push_back(std::move(secret));
    if (differs) secrets.push_back(std::move(escaped));
  }
  return secrets;
}
auto Settings::Redact(std::string text, const std::vector<std::string>& secrets)
    -> std::string {
  constexpr std::string_view replacement = "[redacted]";
  for (const auto& value : secrets) {
    if (value.empty()) continue;
    for (size_t i = 0; (i = text.find(value, i)) != text.npos;) {
      text.replace(i, value.size(), replacement);
      i += replacement.size();
    }
  }
  return text;
}
auto Settings::HostPaths(const std::string& project) -> H::HostPathBindingsT {
  if (!crypto::is_short_id(project))
    throw std::runtime_error("Invalid project id");
  auto& s = *state_;
  std::lock_guard lock(s.mutex);
  H::HostPathBindingsT out;
  auto rows = s.db.prepare(
      "SELECT scope,name,path FROM host_path_bindings WHERE project=? "
      "ORDER BY scope,name");
  rows.bind(1, project);
  while (rows.step()) {
    auto binding = std::make_unique<H::HostPathBindingT>();
    binding->scope = rows.text(0);
    binding->name = rows.text(1);
    binding->path = rows.text(2);
    out.bindings.push_back(std::move(binding));
  }
  return out;
}

auto Settings::CardSecretValues(const std::string& project,
                                const std::vector<std::string>& names)
    -> std::vector<std::pair<std::string, std::string>> {
  auto& s = *state_;
  std::lock_guard lock(s.mutex);
  const auto prefix = CardSecretPrefix(project);
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& name : names) {
    if (!ValidBindingName(name)) continue;
    auto value = s.secrets->Get(prefix + name);
    if (!value.empty()) out.emplace_back(name, std::move(value));
  }
  return out;
}
auto Settings::CachePushSettings()
    -> Tsugumi::RemoteWorker::CachePushSettingsT {
  auto& s = *state_;
  std::lock_guard lock(s.mutex);
  const auto stored = s.CachePushTarget();
  using Kind = Tsugumi::RemoteWorker::CacheTargetKind;
  Tsugumi::RemoteWorker::CachePushSettingsT result;
  switch (stored.kind) {
    case H::CachePushKind::Own:
      result.kind = Kind::Own;
      break;
    case H::CachePushKind::Attic:
      result.kind = Kind::Attic;
      break;
    case H::CachePushKind::Cachix:
      result.kind = Kind::Cachix;
      break;
    case H::CachePushKind::NixCopy:
      result.kind = Kind::NixCopy;
      break;
  }
  result.endpoint = stored.endpoint;
  result.cache_name = stored.cache_name;
  if (stored.has_credentials) {
    result.user = s.secrets->Get(kCachePushUser);
    result.secret = s.secrets->Get(kCachePushSecret);
  }
  return result;
}
auto Settings::AllowUnscopedHelpers() -> bool {
  std::lock_guard lock(state_->mutex);
  const auto values = state_->Values();
  return j::Bool(j::Get(values, "agent.allowUnscopedHelpers"));
}

namespace {
auto MemoryGiB() -> uint32_t {
  const auto pages = ::sysconf(_SC_PHYS_PAGES);
  const auto page = ::sysconf(_SC_PAGESIZE);
  return pages > 0 && page > 0
             ? static_cast<uint32_t>((uint64_t(pages) * uint64_t(page)) >> 30)
             : 8;
}
auto Cores() -> uint32_t {
  return std::max(1u, std::thread::hardware_concurrency());
}
}  // namespace
auto VMDefaultResources() -> H::VMResourcesT {
  H::VMResourcesT out;
  out.cpus = std::clamp(Cores() > 2 ? Cores() - 2 : 0u, 2u, 8u);
  out.memory_gib = std::clamp(MemoryGiB() / 2, 4u, 16u);
  out.store_gib = 64;
  return out;
}
auto VMResourceLimits() -> H::VMResourcesT {
  H::VMResourcesT out;
  out.cpus = Cores();
  out.memory_gib = std::max(2u, MemoryGiB() > 2 ? MemoryGiB() - 2 : 0u);
  out.store_gib = 2048;
  return out;
}
auto Settings::VMResources() -> H::VMResourcesT {
  std::lock_guard lock(state_->mutex);
  const auto values = state_->Values();
  auto out = VMDefaultResources();
  const auto pick = [&](const char* key, uint32_t& field) {
    if (const auto n = j::UInt(j::Get(values, key, "0")))
      field = static_cast<uint32_t>(n);
  };
  pick("vm.cpus", out.cpus);
  pick("vm.memoryGiB", out.memory_gib);
  pick("vm.storeGiB", out.store_gib);
  return out;
}
auto Settings::Language() -> std::string {
  std::lock_guard lock(state_->mutex);
  return j::Text(state_->Values(), "app.language", "ja");
}

auto Settings::ModelSelection(const std::string& profile,
                              const std::string& purpose)
    -> S::ProviderSelectionT {
  if (purpose != "guide" && purpose != "summary")
    throw std::runtime_error("Unknown model purpose");
  auto& s = *state_;
  std::lock_guard lock(s.mutex);
  const auto values = s.Values();
  S::ProviderSelectionT out;
  out.profile = profile;
  out.purpose = purpose;
  out.provider_id = j::Text(values, "agent.provider", "anthropic");
  const auto& id = out.provider_id;
  out.model = j::Text(values, id == "chatgpt"  ? "agent.model.chatgpt"
                              : id == "openai" ? "agent.model.openai"
                                               : "agent.model");
  if (id == "fixture")
    out.model = "fixture";
  else if (id == "chatgpt") {
    const auto override = j::Text(values, "agent.speechModel.chatgpt");
    if (!override.empty())
      out.model = override;
    else {
      const auto offered =
          s.models ? s.models() : host::Models::StoredChatGPTModels(s.db);
      const auto version = [](const std::string& model) {
        std::vector<int> parts;
        int n = -1;
        for (const auto c : model) {
          if (c >= '0' && c <= '9')
            n = (n < 0 ? 0 : n) * 10 + c - '0';
          else if (n >= 0) {
            parts.push_back(n);
            n = -1;
          }
        }
        if (n >= 0) parts.push_back(n);
        return parts;
      };
      std::string newest;
      for (const auto& model : offered)
        if (model.find("luna") != model.npos &&
            (newest.empty() || version(newest) < version(model)))
          newest = model;
      if (std::find(offered.begin(), offered.end(), "gpt-6.1-luna") !=
          offered.end())
        out.model = "gpt-6.1-luna";
      else if (!newest.empty())
        out.model = newest;
    }
  } else {
    if (id == "anthropic") out.model = "claude-haiku-5-5";
    if (purpose == "summary") {
      const auto model =
          j::Text(values, id == "openai" ? "agent.thoughtModel.openai"
                                         : "agent.thoughtModel");
      if (!model.empty()) out.model = model;
    }
  }
  return out;
}

auto Settings::Resolve(const j::Members& selection) -> agent::ProviderConfig {
  const auto purpose = j::Text(selection, "purpose");
  if (!purpose.empty()) {
    const auto selected =
        ModelSelection(j::Text(selection, "profile", "desktop"), purpose);
    auto resolved = selection;
    resolved.erase("purpose");
    resolved["provider_id"] = j::Quote(selected.provider_id);
    resolved["model"] = j::Quote(selected.model);
    resolved["use_choices"] = "false";
    return Resolve(resolved);
  }
  auto& s = *state_;
  std::lock_guard lock(s.mutex);
  auto profile = j::Text(selection, "profile");
  if (profile.empty()) profile = "desktop";
  const auto values = s.Values();
  auto id = j::Text(selection, "provider_id");
  if (id.empty()) id = j::Text(values, "agent.provider", "anthropic");
  if (id != "anthropic" && id != "openai" && id != "chatgpt" && id != "fixture")
    throw std::runtime_error("Unknown provider");
  agent::ProviderConfig c;
  c.api = id == "openai" || id == "chatgpt"
              ? agent::ProviderAPI::OpenAIResponses
              : agent::ProviderAPI::AnthropicMessages;
  c.auth = id == "chatgpt"   ? agent::ProviderAuth::ChatGPT
           : id == "fixture" ? agent::ProviderAuth::None
                             : agent::ProviderAuth::ApiKey;
  c.base_url = id == "anthropic" ? "https://api.anthropic.com"
                                 : "https://api.openai.com";
  c.model = j::Text(values, id == "openai"    ? "agent.model.openai"
                            : id == "chatgpt" ? "agent.model.chatgpt"
                                              : "agent.model");
  if (c.model.empty())
    c.model = j::Text(Defaults(), id == "openai"    ? "agent.model.openai"
                                  : id == "chatgpt" ? "agent.model.chatgpt"
                                                    : "agent.model");
  c.max_tokens = 32768;
  uint32_t input_limit_tokens = 0;
  if (id == "fixture") {
    c.base_url =
        "http://127.0.0.1:" +
        std::to_string(j::UInt(j::Get(values, "agent.fixturePort"), 8765));
    c.model = "fixture";
  }
  auto q = s.db.prepare("SELECT json FROM provider_connections WHERE profile=? AND id=?");
  q.bind(1, profile).bind(2, id);
  if (q.step()) {
    const auto v = j::Object(q.text(0));
    if (!j::Text(v, "baseURL").empty()) c.base_url = j::Text(v, "baseURL");
    if (!j::Text(v, "model").empty()) c.model = j::Text(v, "model");
    input_limit_tokens = j::UInt(j::Get(v, "contextLimit"), input_limit_tokens);
    c.max_tokens = j::UInt(j::Get(v, "maxTokens"), c.max_tokens);
  }
  if (!j::Text(selection, "model").empty())
    c.model = j::Text(selection, "model");
  c.web_search = j::Bool(j::Get(values, "agent.webSearch"));
  for (const auto& domain :
       j::Array(j::Get(values, "agent.webSearch.allowedDomains", "[]")))
    c.search_allowed_domains.push_back(j::String(domain));
  for (const auto& domain :
       j::Array(j::Get(values, "agent.webSearch.blockedDomains", "[]")))
    c.search_blocked_domains.push_back(j::String(domain));
  if (j::Bool(j::Get(selection, "use_choices"))) {
    const auto table = j::Object(
        std::string_view(reinterpret_cast<const char*>(tsugumi_agent_settings),
                         tsugumi_agent_settings_size));
    const auto options = j::Object(j::Get(table, "modelOptions"));
    const auto connections = j::Object(j::Get(options, "connections"));
    if (connections.contains(id)) {
      const auto connection = j::Object(connections.at(id));
      const auto selected = j::Text(values, "agent.effort");
      for (const auto& raw : j::Array(j::Get(connection, "effort", "[]"))) {
        const auto group = j::Object(raw);
        bool offered = false;
        for (const auto& model : j::Array(j::Get(group, "models", "[]")))
          offered |= j::String(model) == c.model;
        if (offered)
          for (const auto& level : j::Array(j::Get(group, "levels", "[]")))
            if (j::String(level) == selected) c.effort = selected;
      }
      c.fast = j::Object(j::Get(connection, "fast", "{}")).contains(c.model) &&
               j::Text(values, "agent.fastModel") == c.model;
      if (c.effort == "xhigh" || c.effort == "max")
        c.max_tokens = std::max<uint32_t>(
            c.max_tokens, j::UInt(j::Get(options, "largeOutputTokens"), 64000));
    }
  }
  if (c.auth == agent::ProviderAuth::ApiKey) {
    const auto status = s.ProviderStatus(profile, id);
    if (!status.configured) throw std::runtime_error(status.message);
    c.credential = s.secrets->Get(profile + ":" + id);
  }
  if (c.auth == agent::ProviderAuth::ChatGPT) {
    auto account_profile = j::Text(selection, "account_profile");
    if (account_profile.empty()) account_profile = profile;
    const auto status = s.Status(account_profile, "");
    std::string client;
    for (const auto& a : status.accounts)
      if (a->active && a->signed_in && a->plan_enabled) client = a->client_id;
    if (client.empty()) {
      const auto status = s.ProviderStatus(account_profile, "chatgpt");
      throw agent::ProviderError(status.message, false, 0, {}, status.code);
    }
    c.base_url = j::Text(s.Account(account_profile, client), "resource",
                         "https://api.openai.com/v1");
    c.access_token = [state = state_.get(), client](std::string_view stale) {
      return state->credentials.AccessToken(
          client, stale, [state](const std::string& profile) {
            state->Publish<H::ProviderChangedT>(profile);
          });
    };
  }
  const auto window = host::Models::ContextWindowTokens(
      s.db, id, c.model, profile, j::Text(selection, "account_profile"));
  const auto margin =
      window / 20;  // Reserve 5% for estimation/protocol overhead.
  if (c.max_tokens >= window - margin)
    throw std::runtime_error(
        "The reply token limit leaves no model input room");
  c.context_limit_tokens = window - c.max_tokens - margin;
  // An explicit input budget may lower the model budget, never enlarge it.
  if (input_limit_tokens)
    c.context_limit_tokens =
        std::min(c.context_limit_tokens, input_limit_tokens);
  return c;
}
}  // namespace tsugumi::host
