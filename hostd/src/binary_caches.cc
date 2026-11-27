#include "host/binary_caches.hh"

#include <curl/curl.h>
#include <sys/stat.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#else
#include <unicode/ubrk.h>
#include <unicode/uchar.h>
#include <unicode/ustring.h>
#endif

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <stdexcept>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::host {
namespace {
namespace j = tsugumi::json;
struct Cache {
  std::string id, name, url, key, username, password, host, scheme;
};
struct CacheInvalid : std::runtime_error {
  using std::runtime_error::runtime_error;
};
auto Invalid(const char* message) -> void { throw CacheInvalid(message); }
auto NameValid(const std::string& name) -> bool {
#ifdef __APPLE__
  auto string = CFStringCreateWithBytes(
      nullptr, reinterpret_cast<const UInt8*>(name.data()), name.size(),
      kCFStringEncodingUTF8, false);
  if (!string) return false;
  auto whitespace =
      CFCharacterSetGetPredefined(kCFCharacterSetWhitespaceAndNewline);
  bool nonempty = false;
  size_t count = 0;
  for (CFIndex offset = 0; offset < CFStringGetLength(string);) {
    nonempty |= !CFCharacterSetIsCharacterMember(
        whitespace, CFStringGetCharacterAtIndex(string, offset));
    const auto range =
        CFStringGetRangeOfComposedCharactersAtIndex(string, offset);
    offset = range.location + range.length;
    ++count;
  }
  CFRelease(string);
#else
  UErrorCode error = U_ZERO_ERROR;
  int32_t length = 0;
  u_strFromUTF8(nullptr, 0, &length, name.data(), name.size(), &error);
  error = U_ZERO_ERROR;
  std::vector<UChar> text(length + 1);
  u_strFromUTF8(text.data(), text.size(), nullptr, name.data(), name.size(),
                &error);
  if (U_FAILURE(error)) return false;
  bool nonempty = std::any_of(text.begin(), text.begin() + length,
                              [](UChar c) { return !u_isUWhiteSpace(c); });
  auto* iterator = ubrk_open(UBRK_CHARACTER, "", text.data(), length, &error);
  if (U_FAILURE(error)) return false;
  size_t count = 0;
  ubrk_first(iterator);
  while (ubrk_next(iterator) != UBRK_DONE) ++count;
  ubrk_close(iterator);
#endif
  return nonempty && count <= 80;
}
auto Parse(std::string_view json) -> std::vector<Cache> {
  std::vector<Cache> caches;
  std::set<std::string> ids, urls;
  std::map<std::string, std::string> credentials;
  const auto entries = j::Array(json);
  if (entries.size() > 32)
    Invalid("At most 32 binary caches may be configured.");
  for (const auto& entry : entries) {
    const auto fields = j::Object(entry);
    auto field = [&](const char* key) {
      schema::v::String value;
      if (schema::decode(j::Get(fields, key), value))
        throw std::runtime_error("binary-caches.json cannot be read");
      return value.value();
    };
    Cache cache{
        field("id"),       field("name"),     field("url"), field("publicKey"),
        field("username"), field("password"), {},           {}};
    std::transform(cache.id.begin(), cache.id.end(), cache.id.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (cache.id.size() != 36 ||
        !std::all_of(cache.id.begin(), cache.id.end(), [&](char c) {
          return c == '-' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
      Invalid("binary-caches.json cannot be read");
    if (!ids.insert(cache.id).second)
      Invalid("Each cache must have a distinct identity.");
    if (!NameValid(cache.name))
      Invalid("Each cache needs a name of at most 80 characters.");
    auto* url = curl_url();
    bool valid = url && curl_url_set(url, CURLUPART_URL, cache.url.c_str(),
                                     0) == CURLUE_OK;
    auto part = [&](CURLUPart which) {
      char* value = nullptr;
      if (url) curl_url_get(url, which, &value, 0);
      std::string result = value ? value : "";
      curl_free(value);
      return result;
    };
    cache.host = part(CURLUPART_HOST);
    cache.scheme = part(CURLUPART_SCHEME);
    valid = valid && !cache.host.empty() &&
            (cache.scheme == "https" || cache.scheme == "http") &&
            part(CURLUPART_USER).empty() && part(CURLUPART_PASSWORD).empty() &&
            cache.url.find_first_of("?#") == std::string::npos &&
            std::all_of(cache.url.begin(), cache.url.end(),
                        [](unsigned char c) { return c > 32 && c != 127; }) &&
            urls.insert(cache.url).second;
    curl_url_cleanup(url);
    if (!valid)
      Invalid(
          "Use distinct HTTP(S) cache endpoints without credentials, query "
          "strings or whitespace.");
    const auto colon = cache.key.find(':');
    std::string decoded;
    if (colon == 0 || colon == cache.key.npos ||
        !std::all_of(cache.key.begin(), cache.key.begin() + colon,
                     [](unsigned char c) {
                       return c < 128 && (std::isalnum(c) ||
                                          std::string_view("._+-").find(c) !=
                                              std::string_view::npos);
                     }) ||
        !crypto::base64_decode(cache.key.substr(colon + 1), decoded) ||
        decoded.size() != 32)
      Invalid(
          "Each cache needs its complete Nix signing public key "
          "(name:base64).");
    if (cache.username.empty() != cache.password.empty())
      Invalid(
          "Provide both an authentication username and password, or leave "
          "both empty.");
    if (!cache.password.empty()) {
      auto printable = [](const std::string& text) {
        return std::all_of(text.begin(), text.end(), [](unsigned char c) {
          return c > 32 && c < 127 && c != 34 && c != 92 && c != 35;
        });
      };
      if (cache.scheme != "https" || !printable(cache.username) ||
          !printable(cache.password) || !printable(cache.host))
        Invalid(
            "Cache credentials require HTTPS and printable values without "
            "whitespace, quotes or backslashes.");
      const auto secret = cache.username + '\n' + cache.password;
      if (credentials.contains(cache.host) &&
          credentials.at(cache.host) != secret)
        Invalid(
            "Caches on the same host must use the same authentication "
            "credentials.");
      credentials[cache.host] = secret;
    }
    caches.push_back(std::move(cache));
  }
  return caches;
}
auto ReadBinaryCaches(const std::filesystem::path& support) -> std::string {
  return fs::read_file(support / "binary-caches.json").value_or("[]");
}
}  // namespace
auto LoadBinaryCaches(const std::filesystem::path& support) -> std::string {
  const auto raw = ReadBinaryCaches(support);
  Parse(raw);
  return raw;
}
auto LoadBinaryCachePulls(const std::filesystem::path& support)
    -> std::vector<BinaryCachePull> {
  std::vector<BinaryCachePull> out;
  for (auto& cache : Parse(ReadBinaryCaches(support)))
    out.push_back({std::move(cache.url), std::move(cache.key),
                   std::move(cache.host), std::move(cache.username),
                   std::move(cache.password)});
  return out;
}
auto SaveBinaryCaches(const std::filesystem::path& support,
                      std::string_view json) -> void {
  Parse(json);
  std::filesystem::create_directories(support);
  ::chmod(support.c_str(), 0700);
  fs::write_atomic(support / "binary-caches.json", json, 0600);
}
auto PrepareBinaryCaches(const std::filesystem::path& support)
    -> BinaryCacheSnapshot {
  BinaryCacheSnapshot result;
  std::vector<Cache> caches;
  try {
    caches = Parse(ReadBinaryCaches(support));
  } catch (const std::exception& error) {
    // Parsing/validation diagnostics never include the submitted credentials.
    result.problem =
        std::string("The binary cache settings were not applied: ") +
        (dynamic_cast<const CacheInvalid*>(&error)
             ? error.what()
             : "binary-caches.json cannot be read") +
        std::string(
            " Only the default caches are used until they are saved "
            "again in Settings.");
    return result;
  }
  if (caches.empty()) return result;
  result.configuration = "extra-substituters =";
  for (const auto& cache : caches) result.configuration += ' ' + cache.url;
  result.configuration += "\nextra-trusted-public-keys =";
  for (const auto& cache : caches) result.configuration += ' ' + cache.key;
  result.configuration += "\nnetrc-file = /run/tsugumi/binary-caches/netrc\n";
  std::set<std::string> hosts;
  for (const auto& cache : caches)
    if (!cache.password.empty() && hosts.insert(cache.host).second)
      result.netrc += "machine " + cache.host + " login " + cache.username +
                      " password " + cache.password + '\n';
  return result;
}
}  // namespace tsugumi::host
