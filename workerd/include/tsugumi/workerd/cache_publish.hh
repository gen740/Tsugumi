#pragma once

#include <filesystem>
#include <stop_token>
#include <string>

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
// invocation or daemon log.
auto publish_to_external_cache(const ExternalCacheTarget& target,
                               const std::filesystem::path& scratch,
                               const CachePublishSource& source,
                               const std::string& store_path,
                               std::stop_token stop) -> void;

}  // namespace tsugumi
