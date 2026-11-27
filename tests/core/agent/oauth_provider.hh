#pragma once

#include <map>
#include <stdexcept>
#include <string_view>

#include <curl/curl.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>

#include "../agent_fixture_server.hh"

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
class OAuthProvider {
 public:
  ModelServer server;
  OAuthProvider() : key_(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", 2048)) {
    if (!key_) throw std::runtime_error("test RSA key generation failed");
  }
  ~OAuthProvider() { EVP_PKEY_free(key_); }
  auto Authorize(const std::string& url, const std::string& client,
                 uint64_t expiry = 3600, bool plan = true)
      -> std::map<std::string, std::string> {
    const auto fields = URLFields(url);
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
        ",\"scope\":" + Quote(plan ? "chatgpt.tokens.use.direct" : "") +
        ",\"expires_in\":" + std::to_string(expiry) + "}");
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
    return fields;
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

 private:
  EVP_PKEY* key_;
};
}  // namespace tsugumi::test
