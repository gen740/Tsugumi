#include "host/settings.hh"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include "agent/wire.hh"
#include "host/models.hh"
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
    auto* n = BN_bin2bn(reinterpret_cast<const unsigned char*>(modulus.data()),
                        modulus.size(), nullptr);
    auto* e = BN_bin2bn(reinterpret_cast<const unsigned char*>(exponent.data()),
                        exponent.size(), nullptr);
    auto* builder = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, n);
    OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, e);
    auto* params = OSSL_PARAM_BLD_to_param(builder);
    auto* context = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_fromdata_init(context) > 0 &&
        EVP_PKEY_fromdata(context, &pkey, EVP_PKEY_PUBLIC_KEY, params) > 0) {
      auto* md = EVP_MD_CTX_new();
      const auto signature = Un64(token.substr(b + 1));
      verified =
          EVP_DigestVerifyInit(md, nullptr, EVP_sha256(), nullptr, pkey) > 0 &&
          EVP_DigestVerify(
              md, reinterpret_cast<const unsigned char*>(signature.data()),
              signature.size(),
              reinterpret_cast<const unsigned char*>(token.data()), b) == 1;
      EVP_MD_CTX_free(md);
    }
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(context);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(builder);
    BN_free(n);
    BN_free(e);
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
  explicit SqlSecrets(sql::Db& db) : db_(db) {
    db_.exec(
        "CREATE TABLE IF NOT EXISTS secrets(id TEXT PRIMARY KEY, value TEXT "
        "NOT NULL)");
  }
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
          {"agent.webSearch", "false"},
          {"agent.animateWork", "true"},
          {"agent.allowUnscopedHelpers", "false"},
          {"agent.permissions.allowed", "[]"},
          {"agent.skillFolders", "[]"},
          {"agent.trustedFolders", "[]"},
          {"versions.noteMode", j::Quote("mechanical")},
          {"vm.cpus", "0"},
          {"vm.memoryGiB", "0"},
          {"vm.storeGiB", "0"},
          {"app.language", j::Quote("ja")}};
}
}  // namespace
struct Settings::State {
  std::filesystem::path directory;
  sql::Db db;
  std::unique_ptr<SecretStore> secrets;
  Import import;
  Token token;
  Forget forget;
  std::recursive_mutex mutex;
  // Import and sign-out serialize without holding the settings lock while
  // entering the credential service (whose callbacks take the Agent lock).
  std::mutex account_operations;
  struct Attempt {
    std::string id, profile, state = "pending", message, welcome, issuer,
                             resource, client, subject, nonce, verifier,
                             oauth_state, redirect;
    int socket = -1;
    std::jthread worker;
  };
  std::map<std::string, std::shared_ptr<Attempt>> attempts;
  std::map<std::string, std::string> latest;
  Models models;
  std::function<void(const std::string&)> provider_changed;
  State(const std::filesystem::path& dir, Import i, Token t, Forget f,
        std::unique_ptr<SecretStore> store, Models model_list, std::function<void(const std::string&)> changed)
      : directory(dir),
        db((dir / "host-settings.sqlite").string()),
        secrets(std::move(store)),
        import(std::move(i)),
        token(std::move(t)),
        forget(std::move(f)),
        models(std::move(model_list)),
        provider_changed(std::move(changed)) {
    ::chmod((dir / "host-settings.sqlite").c_str(), 0600);
    db.exec(
        "PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS settings(profile "
        "TEXT PRIMARY KEY,revision INTEGER NOT NULL,json TEXT NOT NULL); "
        "CREATE TABLE IF NOT EXISTS providers(profile TEXT,id TEXT,json TEXT "
        "NOT NULL,PRIMARY KEY(profile,id)); CREATE TABLE IF NOT EXISTS "
        "accounts(profile TEXT,id TEXT,json TEXT NOT NULL,PRIMARY "
        "KEY(profile,id));");
    db.exec(
        "CREATE TABLE IF NOT EXISTS oauth_hosts(profile TEXT PRIMARY KEY,id "
        "TEXT NOT NULL)");
    if (!secrets) secrets = std::make_unique<SqlSecrets>(db);
  }
  ~State() {
    for (auto& [_, a] : attempts) a->worker.request_stop();
    for (auto& [_, a] : attempts)
      if (a->worker.joinable()) a->worker.join();
  }
  auto Values(const std::string& profile) -> std::pair<uint64_t, j::Members> {
    auto values = Defaults();
    uint64_t revision = 0;
    auto q = db.prepare("SELECT revision,json FROM settings WHERE profile=?");
    q.bind(1, profile);
    if (q.step()) {
      revision = q.u64(0);
      for (const auto& [k, v] : j::Object(q.text(1))) values[k] = v;
    }
    return {revision, values};
  }
  auto Get(const std::string& profile) -> H::SettingsT {
    const auto [rev, values] = Values(profile);
    H::SettingsT out;
    out.revision = rev;
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
  auto SaveAccount(const std::string& profile, const std::string& id,
                   const j::Members& value) -> void {
    const auto encoded = j::Encode(value);
    auto old = db.prepare("SELECT json FROM accounts WHERE profile=? AND id=?");
    old.bind(1, profile).bind(2, id);
    if (old.step() && old.text(0) == encoded) return;
    auto q = db.prepare("INSERT OR REPLACE INTO accounts VALUES(?,?,?)");
    q.bind(1, profile).bind(2, id).bind(3, encoded).run();
    if (provider_changed) provider_changed(profile);
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
  auto Status(const std::string& profile, const std::string& attempt)
      -> H::SignInStatusT {
    H::SignInStatusT out;
    out.state = "idle";
    const auto selected = attempt.empty() && latest.contains(profile)
                              ? latest.at(profile)
                              : attempt;
    if (!selected.empty()) {
      const auto it = attempts.find(selected);
      if (it == attempts.end() || it->second->profile != profile)
        throw std::runtime_error("Unknown sign-in attempt");
      const auto& a = *it->second;
      out.attempt_id = a.id;
      out.state = a.state;
      out.message = a.message;
      out.welcome = a.welcome;
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
      if (a->active && !a->signed_in) {
        if (out.state == "idle" || out.state == "complete")
          out.state = "needs_sign_in";
        out.message = "Sign in to ChatGPT again";
      }
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
  auto Begin(const H::SignInBeginRequest& p) -> H::SignInBeginT {
    auto a = std::make_shared<Attempt>();
    a->profile = p.profile()->str();
    a->id = crypto::uuid4();
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
      previous = Account(a->profile, a->client);
      if (j::Text(previous, "issuer") != a->issuer)
        throw std::runtime_error("Account issuer mismatch");
      a->subject = j::Text(previous, "subject");
    }
    a->socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (a->socket < 0) throw std::runtime_error("Cannot open sign-in callback");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(a->socket, reinterpret_cast<sockaddr*>(&address),
               sizeof address) != 0 ||
        ::listen(a->socket, 4) != 0) {
      ::close(a->socket);
      throw std::runtime_error("Cannot bind sign-in callback");
    }
    socklen_t length = sizeof address;
    ::getsockname(a->socket, reinterpret_cast<sockaddr*>(&address), &length);
    a->redirect =
        "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) +
        "/auth/callback";
    a->oauth_state = crypto::random_token(32);
    a->nonce = crypto::random_token(32);
    a->verifier = crypto::random_token(32);
    auto host_query = db.prepare("SELECT id FROM oauth_hosts WHERE profile=?");
    host_query.bind(1, a->profile);
    auto hostid = host_query.step() ? host_query.text(0) : "";
    if (hostid.empty()) {
      hostid = "urn:uuid:" + crypto::uuid4();
      auto q = db.prepare("INSERT INTO oauth_hosts VALUES(?,?)");
      q.bind(1, a->profile).bind(2, hostid).run();
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
    attempts[a->id] = a;
    latest[a->profile] = a->id;
    a->worker = std::jthread([this, a](std::stop_token stop) {
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
        const auto readable = [&](int fd) {
          pollfd fds[] = {{fd, POLLIN, 0}, {wake.fds[0], POLLIN, 0}};
          int result;
          do {
            result = ::poll(fds, 2, -1);
          } while (result < 0 && errno == EINTR);
          return result > 0 && !stop.stop_requested() &&
                 (fds[0].revents & POLLIN);
        };
        while (!stop.stop_requested()) {
          if (!readable(a->socket)) break;
          const int fd = ::accept(a->socket, nullptr, nullptr);
          if (fd < 0) {
            if (errno == EINTR) continue;
            break;
          }
          ::fcntl(fd, F_SETFD, FD_CLOEXEC);
          std::string input;
          char buffer[2048];
          // Bound the memory a loopback peer can claim for callback headers.
          while (input.size() < 16384 && input.find("\r\n\r\n") == input.npos &&
                 readable(fd)) {
            auto n = ::recv(fd, buffer, sizeof buffer, 0);
            if (n <= 0) break;
            input.append(buffer, n);
          }
          bool valid = false;
          try {
            const auto end = input.find(" HTTP/");
            if (!stop.stop_requested() && input.size() < 16384 &&
                input.find("\r\n\r\n") != input.npos &&
                input.starts_with("GET /auth/callback?") && end != input.npos) {
              cb = Query(std::string_view(input).substr(19, end - 19));
              valid = cb["state"] == a->oauth_state;
            }
          } catch (...) {
          }
          const std::string response =
              valid ? "HTTP/1.1 200 OK\r\nContent-Type: text/html; "
                      "charset=utf-8\r\nCache-Control: "
                      "no-store\r\nReferrer-Policy: no-referrer\r\nConnection: "
                      "close\r\n\r\n<!doctype "
                      "html><title>Tsugumi</title>サインインの結果は Tsugumi "
                      "で確認できます。このタブを閉じて戻ってください。 / "
                      "Check the result in Tsugumi. Close this tab and return."
                    : "HTTP/1.1 404 Not Found\r\nConnection: "
                      "close\r\nContent-Length: 0\r\n\r\n";
          ::send(fd, response.data(), response.size(), 0);
          ::close(fd);
          if (valid) break;
          cb.clear();
        }
        ::close(a->socket);
        a->socket = -1;
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
        credential["token_endpoint"] =
            j::Quote(a->issuer + "/api/accounts/oauth/token");
        credential["revocation_endpoint"] =
            j::Quote(a->issuer + "/api/accounts/oauth/revoke");
        credential["resource"] = j::Quote(a->resource);
        // The issuer may omit expires_in; preserve the documented token
        // lifetime.
        credential["expires_at_ms"] = std::to_string(
            Now() + j::UInt(j::Get(t, "expires_in"), 3600) * 1000);
        std::lock_guard operation(account_operations);
        {
          std::lock_guard guard(mutex);
          if (stop.stop_requested())
            throw std::runtime_error("Sign-in cancelled");
          // Cancellation can win until credential commit begins. Afterwards
          // the completed registration is what status and sign-out observe.
          a->state = "committing";
        }
        import(a->client, credential);
        std::lock_guard guard(mutex);
        bool welcomed = false;
        try {
          welcomed =
              j::Bool(j::Get(Account(a->profile, a->client), "welcomed"));
        } catch (...) {
        }
        SaveAccount(a->profile, a->client,
                    {{"issuer", j::Quote(a->issuer)},
                     {"resource", j::Quote(a->resource)},
                     {"subject", j::Get(claims, "sub")},
                     {"email", j::Get(claims, "email", j::Quote(""))},
                     {"signed_in", "true"},
                     {"plan_enabled", plan ? "true" : "false"},
                     {"welcomed", welcomed ? "true" : "false"}});
        Choose(a->profile, a->client);
        a->state = "complete";
        if (plan && !welcomed) a->welcome = a->client;
      } catch (...) {
        if (a->socket >= 0) {
          ::close(a->socket);
          a->socket = -1;
        }
        std::lock_guard guard(mutex);
        a->state = stop.stop_requested() ? "cancelled" : "failed";
        a->message = stop.stop_requested() ? "Sign-in cancelled"
                                           : "Sign-in failed; try again";
      }
    });
    H::SignInBeginT out;
    out.attempt_id = a->id;
    out.url = a->issuer + "/api/accounts/authorize?" + Form(query);
    return out;
  }
};
Settings::Settings(const std::filesystem::path& d, Import i, Token t, Forget f,
                   std::unique_ptr<SecretStore> store, Models models,
                   std::function<void(const std::string&)> provider_changed)
    : state_(std::make_unique<State>(
          d, std::move(i), std::move(t), std::move(f), std::move(store),
          std::move(models), std::move(provider_changed))) {}
Settings::~Settings() = default;
auto Settings::Handles(H::RequestPayload type) -> bool {
  switch (type) {
    case H::RequestPayload::ModelSelectionRequest:
    case H::RequestPayload::SettingsGetRequest:
    case H::RequestPayload::SettingsUpdateRequest:
    case H::RequestPayload::ProviderSetRequest:
    case H::RequestPayload::ProviderStatusRequest:
    case H::RequestPayload::SignInBeginRequest:
    case H::RequestPayload::SignInActionRequest:
    case H::RequestPayload::SignInStatusRequest:
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
      case H::RequestPayload::SettingsUpdateRequest: {
        const auto& p = *request.value_as_SettingsUpdateRequest();
        sql::Transaction tx(s.db);
        auto [revision, values] = s.Values(p.profile()->str());
        if (revision != p.revision()) {
          S::FailureT f;
          f.code = "conflict";
          f.message = "Settings changed; read the latest revision";
          return agent::wire::HostReply(id, std::move(f));
        }
        if (p.changes())
          for (const auto* item : *p.changes()) {
            const auto key = item->key()->str();
            auto value = item->value_json()->str();
            if (key == "agent.permissions.allowed" &&
                Text(p.client()) == "web") {
              // The Web screen edits four kinds; preserve the app-only Nix
              // kind, as the former desktop preference adapter did.
              const std::vector<std::string> kinds = {"network", "sandbox_off",
                                                      "host_folder_read",
                                                      "host_folder_write"};
              std::vector<std::string> merged;
              for (const auto& raw : j::Array(j::Get(values, key, "[]")))
                if (std::find(kinds.begin(), kinds.end(), j::String(raw)) ==
                    kinds.end())
                  merged.push_back(raw);
              for (const auto& raw : j::Array(value))
                if (std::find(kinds.begin(), kinds.end(), j::String(raw)) !=
                    kinds.end())
                  merged.push_back(raw);
              value = j::Encode(merged);
            }
            if (value.size() > 65536 || key == "agent.oauthHostID" ||
                !(key.starts_with("agent.") || key.starts_with("vm.") ||
                  key == "versions.noteMode" || key == "app.language"))
              throw std::runtime_error("Invalid setting");
            j::Object("{\"value\":" + value + "}");
            const auto defaults = Defaults();
            if (!defaults.contains(key) && key != "agent.drawThoughts" &&
                key != "agent.drawThoughtsAsked" &&
                key != "agent.thoughtModel" &&
                key != "agent.thoughtModel.openai" &&
                key != "agent.speechModel.chatgpt")
              throw std::runtime_error("Unknown setting");
            if (key == "app.language" && j::String(value) != "ja" &&
                j::String(value) != "en")
              throw std::runtime_error("Unknown language");
            if (key == "agent.trustedFolders") {
              if (p.profile()->str() != "desktop")
                throw std::runtime_error(
                    "Trusted folders belong to the host profile");
              const auto previous = j::Array(j::Get(values, key, "[]"));
              std::vector<std::string> folders;
              for (const auto& raw : j::Array(value)) {
                const auto path = std::filesystem::path(j::String(raw));
                const auto registered = std::any_of(
                    previous.begin(), previous.end(), [&](const auto& old) {
                      return j::String(old) == path.string();
                    });
                if (!path.is_absolute() ||
                    (!registered && !std::filesystem::is_directory(path)))
                  throw std::runtime_error(
                      "Choose an existing absolute folder");
                // Preserve existing canonical registrations even if a folder
                // is missing or replaced by a link while another entry is
                // edited.
                const auto canonical =
                    registered ? path.string()
                               : std::filesystem::canonical(path).string();
                const auto encoded = j::Quote(canonical);
                if (std::find(folders.begin(), folders.end(), encoded) ==
                    folders.end())
                  folders.push_back(encoded);
              }
              value = j::Encode(folders);
            }
            if (key.starts_with("vm.")) {
              if (p.profile()->str() != "desktop")
                throw std::runtime_error("The VM belongs to the host profile");
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
            if (key == "agent.permissions.allowed")
              for (const auto& kind : j::Array(value))
                if (j::String(kind).empty())
                  throw std::runtime_error("Invalid permission kind");
            if (key == "versions.noteMode" &&
                j::String(value) != "mechanical" && j::String(value) != "agent")
              throw std::runtime_error("Unknown version note mode");
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
            values[key] = value;
          }
        auto q = s.db.prepare("INSERT OR REPLACE INTO settings VALUES(?,?,?)");
        q.bind(1, p.profile()->str())
            .bind(2, revision + 1)
            .bind(3, j::Encode(values))
            .run();
        tx.commit();
        return agent::wire::HostReply(id, s.Get(p.profile()->str()));
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
              s.db.prepare("DELETE FROM providers WHERE profile=? AND id=?");
          q.bind(1, profile).bind(2, provider).run();
        } else if (p.provider()) {
          auto value = j::Object(agent::wire::ProviderJson(*p.provider()));
          const auto endpoint = j::Text(value, "baseURL");
          if (endpoint.empty() && j::Text(value, "model").empty()) {
            auto old = s.db.prepare(
                "SELECT json FROM providers WHERE profile=? AND id=?");
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
              s.db.prepare("INSERT OR REPLACE INTO providers VALUES(?,?,?)");
          q.bind(1, profile).bind(2, provider).bind(3, j::Encode(value)).run();
        }
        const auto after = s.ProviderStatus(profile, provider);
        if (s.provider_changed && (before.configured != after.configured ||
                                   before.message != after.message ||
                                   before.endpoint != after.endpoint ||
                                   before.source != after.source))
          s.provider_changed(profile);
        return agent::wire::HostReply(id, after);
      }
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
          auto it = s.attempts.find(attempt);
          if (it == s.attempts.end() || it->second->profile != profile)
            throw std::runtime_error("Unknown sign-in attempt");
          if (it->second->state == "pending") {
            it->second->worker.request_stop();
            it->second->state = "cancelled";
          }
        } else {
          auto value = s.Account(profile, client);
          if (action == "select")
            s.Choose(profile, client);
          else if (action == "welcomed") {
            value["welcomed"] = "true";
            s.SaveAccount(profile, client, value);
            for (auto& [_, pending] : s.attempts)
              if (pending->profile == profile && pending->welcome == client)
                pending->welcome.clear();
          } else if (action == "signout") {
            for (auto& [_, pending] : s.attempts)
              if (pending->profile == profile && pending->state == "pending") {
                pending->worker.request_stop();
                pending->state = "cancelled";
              }
            lock.unlock();
            std::lock_guard operation(s.account_operations);
            revoked = s.forget(client);
            lock.lock();
            // A registration commit may have completed while we waited.
            value = s.Account(profile, client);
            value["signed_in"] = "false";
            s.SaveAccount(profile, client, value);
          } else
            throw std::runtime_error("Unknown sign-in action");
        }
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
  auto& s = *state_;
  std::lock_guard lock(s.mutex);
  auto remove = [&](const std::string& secret) {
    if (secret.empty()) return;
    const auto encoded = j::Quote(secret);
    const auto escaped = encoded.substr(1, encoded.size() - 2);
    for (const auto& value : {secret, escaped})
      for (size_t i = 0; (i = text.find(value, i)) != text.npos;) {
        text.replace(i, value.size(), "[redacted]");
        i += 10;
      }
  };
  auto rows = s.db.prepare("SELECT profile,id FROM providers");
  while (rows.step()) remove(s.secrets->Get(rows.text(0) + ":" + rows.text(1)));
  return text;
}
auto Settings::AllowUnscopedHelpers(const std::string& profile) -> bool {
  std::lock_guard lock(state_->mutex);
  const auto values = state_->Values(profile).second;
  return j::Bool(j::Get(values, "agent.allowUnscopedHelpers"));
}

auto Settings::TrustedFolders() -> std::vector<std::string> {
  std::lock_guard lock(state_->mutex);
  const auto [revision, values] = state_->Values("desktop");
  std::vector<std::string> folders;
  for (const auto& raw : j::Array(j::Get(values, "agent.trustedFolders", "[]")))
    folders.push_back(j::String(raw));
  return folders;
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
  const auto values = state_->Values("desktop").second;
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
auto Settings::Language(const std::string& profile) -> std::string {
  std::lock_guard lock(state_->mutex);
  return j::Text(state_->Values(profile).second, "app.language", "ja");
}

auto Settings::VersionNoteMode(const std::string& profile) -> std::string {
  std::lock_guard lock(state_->mutex);
  return j::Text(state_->Values(profile).second, "versions.noteMode",
                 "mechanical");
}

auto Settings::ModelSelection(const std::string& profile,
                              const std::string& purpose)
    -> S::ProviderSelectionT {
  if (purpose != "guide" && purpose != "summary")
    throw std::runtime_error("Unknown model purpose");
  auto& s = *state_;
  std::lock_guard lock(s.mutex);
  const auto values = s.Values(profile).second;
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
      const auto offered = s.models ? s.models() : std::vector<std::string>{};
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

auto Settings::VersionNoteProvider(const std::string& profile)
    -> agent::ProviderConfig {
  return Resolve(
      {{"profile", j::Quote(profile)}, {"purpose", j::Quote("summary")}});
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
  const auto values = s.Values(profile).second;
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
  auto q = s.db.prepare("SELECT json FROM providers WHERE profile=? AND id=?");
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
      try {
        return state->token(client, stale);
      } catch (const agent::ProviderError& error) {
        if (error.sign_in_required || error.code == "chatgpt_plan_disabled") {
          // Credentials have already been persisted and its refresh lock has
          // been released. Account presentation belongs to the settings DB.
          std::lock_guard lock(state->mutex);
          auto q =
              state->db.prepare("SELECT profile,json FROM accounts WHERE id=?");
          q.bind(1, client);
          std::vector<std::pair<std::string, j::Members>> accounts;
          while (q.step()) {
            auto account = j::Object(q.text(1));
            account[error.sign_in_required ? "signed_in" : "plan_enabled"] =
                "false";
            accounts.emplace_back(q.text(0), std::move(account));
          }
          for (const auto& [profile, account] : accounts)
            state->SaveAccount(profile, client, account);
        }
        throw;
      }
    };
  }
  const auto window = host::Models::ContextWindowTokens(
      s.directory, id, c.model, profile, j::Text(selection, "account_profile"));
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
