#pragma once
#include <curl/curl.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>

#include "fixture.hh"

namespace tsugumi::test {
inline auto Base64URL(std::string_view bytes) -> std::string {
  std::string out(4 * ((bytes.size() + 2) / 3) + 1, '\0');
  out.resize(EVP_EncodeBlock(
      reinterpret_cast<unsigned char*>(out.data()),
      reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
  for (auto& c : out) {
    if (c == '+') c = '-';
    if (c == '/') c = '_';
  }
  while (out.ends_with('=')) out.pop_back();
  return out;
}
inline auto URLDecode(const std::string& value) -> std::string {
  auto* curl = curl_easy_init();
  int n = 0;
  auto* raw = curl_easy_unescape(curl, value.c_str(), value.size(), &n);
  std::string out(raw, n);
  curl_free(raw);
  curl_easy_cleanup(curl);
  return out;
}
inline auto URLFields(const std::string& url)
    -> std::map<std::string, std::string> {
  std::map<std::string, std::string> out;
  auto query = url.substr(url.find('?') + 1);
  while (!query.empty()) {
    const auto end = query.find('&');
    const auto field = query.substr(0, end);
    const auto equal = field.find('=');
    out[field.substr(0, equal)] = URLDecode(field.substr(equal + 1));
    if (end == query.npos) break;
    query = query.substr(end + 1);
  }
  return out;
}
inline auto SettingsGet(client::Connection& host,
                        const std::string& profile = "desktop") -> Reply {
  return Call(host, H::RequestPayload::SettingsGetRequest, [&](Builder& b) {
    return H::CreateSettingsGetRequest(b, b.CreateString(profile)).Union();
  });
}
inline auto SettingsPatch(client::Connection& host, const std::string& profile,
                          const std::map<std::string, std::string>& change)
    -> Reply {
  const auto before = SettingsGet(host, profile);
  const auto revision = before.HostValue()->value_as_Settings()->revision();
  return Call(host, H::RequestPayload::SettingsUpdateRequest, [&](Builder& b) {
    std::vector<flatbuffers::Offset<H::Setting>> changes;
    for (const auto& [k, v] : change)
      changes.push_back(
          H::CreateSetting(b, b.CreateString(k), b.CreateString(v)));
    return H::CreateSettingsUpdateRequest(b, b.CreateString(profile), revision,
                                          b.CreateVector(changes))
        .Union();
  });
}
class OAuthServer {
 public:
  ModelServer server;
  OAuthServer() : key_(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", 2048)) {
    if (!key_) throw std::runtime_error("test RSA key generation failed");
  }
  ~OAuthServer() { EVP_PKEY_free(key_); }
  auto Begin(client::Connection& host, const std::string& profile = "desktop")
      -> Reply {
    return Call(host, H::RequestPayload::SignInBeginRequest, [&](Builder& b) {
      return H::CreateSignInBeginRequest(b, b.CreateString(profile), 0, false,
                                         b.CreateString(server.URL()),
                                         b.CreateString(resource))
          .Union();
    });
  }
  auto Status(client::Connection& host, const std::string& attempt,
              const std::string& profile = "desktop") -> Reply {
    return Call(host, H::RequestPayload::SignInStatusRequest, [&](Builder& b) {
      return H::CreateSignInStatusRequest(b, b.CreateString(profile),
                                          b.CreateString(attempt))
          .Union();
    });
  }
  auto SignIn(client::Connection& host, const std::string& endpoint,
              const std::string& profile = "desktop", uint64_t expiry = 3600)
      -> Reply {
    resource = endpoint;
    const auto begin = Begin(host, profile);
    if (!begin.Code().empty()) throw std::runtime_error(begin.Code());
    const auto* info = begin.HostValue()->value_as_SignInBegin();
    const auto fields = URLFields(info->url()->str());
    const auto client = "client-" + profile;
    const auto expiration =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count() +
        3600;
    const auto claims =
        "{\"iss\":" + Quote(server.URL()) + ",\"aud\":" + Quote(client) +
        ",\"sub\":\"subject\",\"email\":\"user@example.test\",\"nonce\":" +
        Quote(bad_nonce ? "wrong-nonce" : fields.at("nonce")) +
        ",\"exp\":" + std::to_string(expiration) + "}";
    auto token = Sign(claims);
    if (bad_signature) {
      auto& c = token[token.rfind('.') + 1];
      c = c == 'A' ? 'B' : 'A';
    }
    server.QueueHTTP(
        "{\"access_token\":\"ACCESS_SENTINEL\",\"refresh_token\":\"REFRESH_"
        "SENTINEL\",\"id_token\":" +
        Quote(token) +
        ",\"scope\":\"chatgpt.tokens.use.direct\",\"expires_in\":" +
        std::to_string(expiry) + "}");
    server.QueueHTTP(JWKS());
    auto* curl = curl_easy_init();
    const auto callback = fields.at("redirect_uri") +
                          "?state=" + fields.at("state") +
                          "&client_id=" + client + "&code=CODE_SENTINEL";
    curl_easy_setopt(curl, CURLOPT_URL, callback.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION,
        +[](char*, size_t n, size_t m, void*) -> size_t { return n * m; });
    const auto rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) throw std::runtime_error("test callback failed");
    for (int i = 0; i < 500; ++i) {
      auto status = Status(host, info->attempt_id()->str(), profile);
      const auto state =
          status.HostValue()->value_as_SignInStatus()->state()->str();
      if (state != "pending" && state != "committing") {
        const auto exchange = URLFields("?" + server.Requests().at(0));
        const auto& verifier = exchange.at("code_verifier");
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned length = 0;
        EVP_Digest(verifier.data(), verifier.size(), digest, &length,
                   EVP_sha256(), nullptr);
        if (fields.at("code_challenge_method") != "S256" ||
            Base64URL({reinterpret_cast<const char*>(digest), length}) !=
                fields.at("code_challenge") ||
            exchange.at("redirect_uri") != fields.at("redirect_uri") ||
            exchange.at("client_id") != client ||
            exchange.at("resource") != resource)
          throw std::runtime_error(
              "test OAuth exchange did not bind PKCE and registration");
        return status;
      }
      std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("test sign-in timed out");
  }
  auto Sign(const std::string& claims) -> std::string {
    const auto unsigned_token =
        Base64URL(R"({"alg":"RS256","kid":"fixture"})") + '.' +
        Base64URL(claims);
    auto* md = EVP_MD_CTX_new();
    EVP_DigestSignInit(md, nullptr, EVP_sha256(), nullptr, key_);
    size_t length = 0;
    EVP_DigestSign(
        md, nullptr, &length,
        reinterpret_cast<const unsigned char*>(unsigned_token.data()),
        unsigned_token.size());
    std::string signature(length, '\0');
    EVP_DigestSign(
        md, reinterpret_cast<unsigned char*>(signature.data()), &length,
        reinterpret_cast<const unsigned char*>(unsigned_token.data()),
        unsigned_token.size());
    EVP_MD_CTX_free(md);
    signature.resize(length);
    return unsigned_token + '.' + Base64URL(signature);
  }
  auto JWKS() -> std::string {
    auto number = [&](const char* name) {
      BIGNUM* n = nullptr;
      EVP_PKEY_get_bn_param(key_, name, &n);
      std::string bytes(BN_num_bytes(n), '\0');
      BN_bn2bin(n, reinterpret_cast<unsigned char*>(bytes.data()));
      BN_free(n);
      return Base64URL(bytes);
    };
    return "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"fixture\",\"n\":" +
           Quote(number(OSSL_PKEY_PARAM_RSA_N)) +
           ",\"e\":" + Quote(number(OSSL_PKEY_PARAM_RSA_E)) + "}]}";
  }
  bool bad_nonce = false;
  bool bad_signature = false;
  std::string resource;

 private:
  EVP_PKEY* key_;
};
}  // namespace tsugumi::test
