#include "tsugumi/workerd/cache_publish.hh"

#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include "tsugumi/util/fs.hh"
#include "tsugumi/util/process.hh"

namespace tsugumi {
namespace {
auto ReadEnvironment(const char* name) -> std::string {
  const auto* value = std::getenv(name);
  return value ? value : "";
}

auto Executable(const std::string& name, const std::string& path)
    -> std::string {
  std::istringstream dirs(path);
  std::string dir;
  while (std::getline(dirs, dir, ':')) {
    const auto candidate = std::filesystem::path(dir) / name;
    if (candidate.is_absolute() && ::access(candidate.c_str(), X_OK) == 0)
      return candidate.string();
  }
  throw std::runtime_error(name + " is unavailable for cache publishing");
}

struct PrivateConfig {
  std::filesystem::path root;
  explicit PrivateConfig(const std::filesystem::path& scratch) {
    auto pattern = (scratch / "cache-publish.XXXXXX").string();
    if (!::mkdtemp(pattern.data()))
      throw std::runtime_error("cannot create a private cache configuration");
    root = pattern;
  }
  ~PrivateConfig() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};

auto NetrcOption(const ExternalCacheTarget& target, const PrivateConfig& cfg)
    -> std::string {
  if (target.secret.empty() || !target.endpoint.starts_with("https://"))
    return {};
  auto host = target.endpoint.substr(std::string_view("https://").size());
  host.resize(host.find('/') == host.npos ? host.size() : host.find('/'));
  if (const auto bracket = host.find(']'); bracket != host.npos)
    host.resize(bracket + 1);
  else if (const auto colon = host.find(':'); colon != host.npos)
    host.resize(colon);
  const auto netrc = cfg.root / "netrc";
  fs::write_atomic(netrc,
                   "machine " + host + " login " + target.user + " password " +
                       target.secret + '\n',
                   0600);
  return "netrc-file = " + netrc.string() + '\n';
}
}  // namespace

auto publish_to_external_cache(const ExternalCacheTarget& target,
                               const std::filesystem::path& scratch,
                               const CachePublishSource& source,
                               const std::vector<std::string>& store_paths,
                               std::stop_token stop) -> void {
  if (store_paths.empty()) return;
  if (stop.stop_requested())
    throw std::runtime_error("cache publishing cancelled");
  const PrivateConfig config(scratch);
  const auto path = ReadEnvironment("PATH");
  proc::Spec spec;
  spec.env = {"HOME=" + config.root.string(), "PATH=" + path,
              "XDG_CONFIG_HOME=" + config.root.string(),
              "XDG_CACHE_HOME=" + config.root.string()};
  if (!source.store.empty()) spec.env.push_back("NIX_REMOTE=" + source.store);
  std::string nix_options;
  // The store is opened in the job's user namespace, where the machine's
  // build-users-group is not mapped.
  if (source.private_worker_store) nix_options = "build-users-group =\n";
  for (const auto* name : {"SSL_CERT_FILE", "NIX_SSL_CERT_FILE"}) {
    const auto value = ReadEnvironment(name);
    if (!value.empty()) spec.env.push_back(std::string(name) + '=' + value);
  }
  switch (target.kind) {
    case ExternalCacheKind::attic:
      std::filesystem::create_directory(config.root / "attic");
      // hostd validates the fields for inclusion in a private config file.
      fs::write_atomic(config.root / "attic/config.toml",
                       "default-server = \"tsugumi\"\n[servers.tsugumi]\n"
                       "endpoint = \"" +
                           target.endpoint + "\"\ntoken = \"" + target.secret +
                           "\"\n",
                       0600);
      spec.argv = {Executable("attic", path), "push", "--no-closure",
                   target.name};
      break;
    case ExternalCacheKind::cachix:
      spec.argv = {Executable("cachix", path), "push", target.name};
      if (!target.secret.empty())
        spec.env.push_back("CACHIX_AUTH_TOKEN=" + target.secret);
      break;
    case ExternalCacheKind::nix_copy:
      spec.argv = {Executable("nix", path), "copy", "--to", target.endpoint};
      nix_options += "experimental-features = nix-command\n";
      nix_options += NetrcOption(target, config);
      if (!target.secret.empty() && target.endpoint.starts_with("s3://")) {
        spec.env.push_back("AWS_ACCESS_KEY_ID=" + target.user);
        spec.env.push_back("AWS_SECRET_ACCESS_KEY=" + target.secret);
      }
      break;
  }
  spec.argv.insert(spec.argv.end(), store_paths.begin(), store_paths.end());
  if (!nix_options.empty()) spec.env.push_back("NIX_CONFIG=" + nix_options);
  std::atomic<bool> cancelled{false};
  std::stop_callback cancel(stop, [&] { cancelled = true; });
  const auto result = proc::run(spec, &cancelled, {}, {});
  if (result.cancelled) throw std::runtime_error("cache publishing cancelled");
  if (!proc::exited_ok(result.status))
    throw std::runtime_error("binary cache upload failed");
}

}  // namespace tsugumi
