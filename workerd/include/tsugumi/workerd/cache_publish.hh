#pragma once

#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace tsugumi {

enum class ExternalCacheKind { attic, cachix, nix_copy };

struct ExternalCacheTarget {
  ExternalCacheKind kind;
  std::string endpoint;
  std::string name;
  std::string user;
  std::string secret;
};

struct CachePublishSource {
  std::string store;
  bool private_worker_store = false;
};

// The target is validated by hostd before delivery. The child sees a private
// configuration and a minimal environment. Its output is never sent to an
// invocation or daemon log. Attic uploads exactly `store_paths` (references
// first), not their closures, so the caller leaves out what pull caches serve.
// Cachix also uploads the references its cache and upstream lack. Nix copy
// uploads the closures minus what the target holds: Nix's binary cache stores
// refuse a path whose references they do not hold.
auto publish_to_external_cache(const ExternalCacheTarget& target,
                               const std::filesystem::path& scratch,
                               const CachePublishSource& source,
                               const std::vector<std::string>& store_paths,
                               std::stop_token stop) -> void;

}  // namespace tsugumi
