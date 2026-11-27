#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

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
auto publish_to_relay_cache(const std::string& source_store,
                            const std::string& store_path,
                            CacheRelay::Exchange exchange) -> void;

}  // namespace tsugumi
