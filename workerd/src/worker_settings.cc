#include "worker_settings.hh"

#include <arpa/inet.h>
#include <flatbuffers/idl.h>

#include <algorithm>
#include <set>
#include <stdexcept>

#include "tsugumi/util/fs.hh"
#include "tsugumi/util/text.hh"

extern const unsigned char tsugumi_worker_settings_fbs[];
extern const unsigned long tsugumi_worker_settings_fbs_size;

namespace tsugumi::workerd {
namespace {
// What Nix accepts in a substituters or trusted-public-keys list entry, and
// what the job's pull list checks again before it configures Nix.
auto CacheField(std::string_view field) -> bool {
  return !field.empty() && field.size() <= 1024 &&
         std::ranges::all_of(field, [](unsigned char c) {
           return c > 32 && c < 127 && c != '"' && c != '\\' && c != '#';
         });
}
auto Refuse(const std::string& message) -> void {
  throw std::invalid_argument(message);
}
auto Quote(std::string_view text) -> std::string {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "\"";
  for (const unsigned char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 32) {
      out += "\\u00";
      out += kHex[c >> 4];
      out += kHex[c & 15];
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + '"';
}
// "3: 14: error: unknown field: x" -> "line 3, column 14: unknown field: x".
auto Describe(const std::string& error) -> std::string {
  const auto mark = error.find("error: ");
  if (mark == std::string::npos) return error;
  auto message = error.substr(mark + 7);
  while (!message.empty() && (message.back() == '\n' || message.back() == ' '))
    message.pop_back();
  auto where = error.substr(0, mark);
  while (!where.empty() && (where.back() == ' ' || where.back() == ':'))
    where.pop_back();
  const auto colon = where.find(':');
  if (colon == std::string::npos) return message;
  auto column = where.substr(colon + 1);
  column.erase(0, column.find_first_not_of(' '));
  return "line " + where.substr(0, colon) + ", column " + column + ": " +
         message;
}
}  // namespace

WorkerSettings::WorkerSettings(const std::filesystem::path& root)
    : file_(root / "settings.json") {
  if (const auto text = fs::read_file(file_)) {
    try {
      saved_ = parse(*text);
      validate(saved_);
    } catch (const std::exception& e) {
      throw std::runtime_error(file_.string() + ": " + e.what());
    }
  } else {
    saved_ = defaults();
    fs::write_atomic(file_, print(saved_), 0600);
  }
  started_ = saved_;
  max_jobs_.store(saved_.max_jobs);
}

auto WorkerSettings::defaults() -> WS::SettingsT {
  WS::SettingsT settings;
  settings.listen_address = "127.0.0.1";
  return settings;
}

auto WorkerSettings::saved() const -> WS::SettingsT {
  std::lock_guard guard(mutex_);
  return saved_;
}

auto WorkerSettings::save(WS::SettingsT settings) -> void {
  validate(settings);
  std::lock_guard guard(mutex_);
  fs::write_atomic(file_, print(settings), 0600);
  max_jobs_.store(settings.max_jobs);
  saved_ = std::move(settings);
}

auto WorkerSettings::restart_required() const -> bool {
  std::lock_guard guard(mutex_);
  return saved_.listen_address != started_.listen_address ||
         saved_.port != started_.port ||
         saved_.dashboard_port != started_.dashboard_port ||
         saved_.nix_max_jobs != started_.nix_max_jobs ||
         saved_.nix_cores != started_.nix_cores;
}

auto WorkerSettings::validate(const WS::SettingsT& settings) -> void {
  const auto& name = settings.name;
  if (name.size() > 128 || !text::valid_utf8(name) ||
      std::ranges::any_of(name,
                          [](unsigned char c) { return c < 32 || c == 127; }))
    Refuse("The display name must be at most 128 bytes of text on one line");
  unsigned char address[16];
  if (::inet_pton(AF_INET, settings.listen_address.c_str(), address) != 1 &&
      ::inet_pton(AF_INET6, settings.listen_address.c_str(), address) != 1)
    Refuse("The listen address must be an IP address");
  if (settings.port && settings.port == settings.dashboard_port)
    Refuse("The dashboard needs a port of its own");
  if (settings.max_jobs < 1 || settings.max_jobs > 65536)
    Refuse("Concurrent jobs must be between 1 and 65536");
  if (settings.nix_max_jobs > 1024)
    Refuse("Nix max-jobs must be between 0 and 1024");
  if (settings.nix_cores > 4096) Refuse("Nix cores must be between 0 and 4096");
  if (settings.pull_caches.size() > 16) Refuse("Add at most 16 pull caches");
  std::set<std::string_view> urls;
  for (const auto& cache : settings.pull_caches) {
    if (!cache) Refuse("A pull cache is missing");
    const auto& url = cache->url;
    if (!CacheField(url) || url.find_first_of("?@") != url.npos ||
        !(url.starts_with("https://") || url.starts_with("http://") ||
          url.starts_with("file:///")) ||
        url.size() < 9)
      Refuse("Pull cache " + url +
             " must be an https://, http:// or file:/// URL without "
             "credentials or a query");
    if (!urls.insert(url).second)
      Refuse("Pull cache " + url + " is listed twice");
    const auto& key = cache->public_key;
    const auto colon = key.find(':');
    if (!CacheField(key) || colon == 0 || colon == key.npos ||
        colon + 1 == key.size())
      Refuse("The public key of " + url + " must have the form name:base64");
  }
}

auto WorkerSettings::parse(std::string_view text) -> WS::SettingsT {
  flatbuffers::IDLOptions options;
  options.strict_json = true;
  options.natural_utf8 = true;
  options.no_warnings = true;
  flatbuffers::Parser parser(options);
  const std::string schema(
      reinterpret_cast<const char*>(tsugumi_worker_settings_fbs),
      tsugumi_worker_settings_fbs_size);
  if (!parser.Parse(schema.c_str()))
    throw std::logic_error("worker settings schema: " + parser.error_);
  const std::string owned(text);
  if (owned.find('\0') != std::string::npos)
    throw std::runtime_error("the file contains a NUL byte");
  if (!parser.ParseJson(owned.c_str()))
    throw std::runtime_error(Describe(parser.error_));
  WS::SettingsT settings;
  WS::GetSettings(parser.builder_.GetBufferPointer())->UnPackTo(&settings);
  return settings;
}

auto WorkerSettings::print(const WS::SettingsT& settings) -> std::string {
  std::string text = "{\n";
  text += "  \"name\": " + Quote(settings.name) + ",\n";
  text += "  \"listen_address\": " + Quote(settings.listen_address) + ",\n";
  text += "  \"port\": " + std::to_string(settings.port) + ",\n";
  text +=
      "  \"dashboard_port\": " + std::to_string(settings.dashboard_port) + ",\n";
  text += "  \"max_jobs\": " + std::to_string(settings.max_jobs) + ",\n";
  text += "  \"nix_max_jobs\": " + std::to_string(settings.nix_max_jobs) + ",\n";
  text += "  \"nix_cores\": " + std::to_string(settings.nix_cores) + ",\n";
  text += "  \"pull_caches\": [";
  for (size_t i = 0; i < settings.pull_caches.size(); ++i) {
    const auto& cache = *settings.pull_caches[i];
    text += i ? ",\n" : "\n";
    text += "    {\n      \"url\": " + Quote(cache.url) +
            ",\n      \"public_key\": " + Quote(cache.public_key) + "\n    }";
  }
  text += settings.pull_caches.empty() ? "]\n" : "\n  ]\n";
  return text + "}\n";
}

}  // namespace tsugumi::workerd
