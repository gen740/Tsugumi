#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "tsugumi/workerd/cache_transfer.hh"

namespace nix {
class Store;
}

namespace tsugumi {

// Speaks in binary-cache objects while carrying bytes over the host's
// authenticated route. The caller supplies a fresh channel for each request.
struct CacheRelay {
  using Sink = std::function<void(std::string_view)>;
  using Source = std::function<size_t(std::span<char>)>;
  enum class Method { head, get, put };
  using Exchange =
      std::function<bool(Method method, const std::string& key,
                         uint64_t request_size, Source upload, Sink download)>;
};

auto open_relay_cache(CacheRelay::Exchange exchange)
    -> std::shared_ptr<nix::Store>;
// Uploads the planned paths to a store from open_relay_cache, one at a time,
// references first. A path's references need not be in the built-in cache: a
// pull cache serves those that were left out.
auto copy_to_relay_cache(nix::Store& source, nix::Store& relay,
                         const CachePushPlan& plan,
                         const CacheTransferReport& report) -> void;
// Uploads the part of the closure that neither a pull cache nor the built-in
// cache already has.
auto publish_to_relay_cache(const std::string& source_store,
                            const std::string& store_path,
                            CacheRelay::Exchange exchange,
                            const CacheTransferReport& report = {})
    -> CachePushPlan;
// The part of the closure that no pull cache serves, for an external target.
auto plan_store_push(const std::string& source_store,
                     const std::string& store_path) -> CachePushPlan;

}  // namespace tsugumi
