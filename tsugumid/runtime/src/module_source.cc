#include "module_source.hh"

#include <mutex>
#include <stdexcept>

#include "tsugumi/util/fs.hh"

namespace tsugumi {
auto ModuleIdentity::parse(std::string_view path)
    -> std::optional<ModuleIdentity> {
  if (!valid_module_identity(path)) return std::nullopt;
  return ModuleIdentity(std::filesystem::path(path));
}

auto cached_module_source(const std::filesystem::path& cache,
                          const ModuleIdentity& identity)
    -> std::optional<std::string> {
  const auto rel = identity.path().filename() / "index.js";
  if (!std::filesystem::exists(cache / rel)) return std::nullopt;
  std::string source;
  fs::stream_beneath(cache, rel, kModuleSourceLimit,
                     [&](std::string_view chunk) { source.append(chunk); });
  return source;
}

auto save_module_source(const std::filesystem::path& cache,
                        const ModuleIdentity& identity, std::string_view source)
    -> void {
  if (source.size() > kModuleSourceLimit)
    throw std::runtime_error("module entry exceeds 32 MiB");
  // Concurrent imports may share an immutable module, never replace its bytes.
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  if (auto saved = cached_module_source(cache, identity)) {
    if (*saved != source)
      throw std::runtime_error("conflicting module source for identity");
    return;
  }
  fs::write_atomic(cache / identity.path().filename() / "index.js", source,
                   0600);
}
}  // namespace tsugumi
