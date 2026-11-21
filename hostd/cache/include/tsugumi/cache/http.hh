// The Nix binary-cache HTTP protocol over one authenticated byte stream.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "tsugumi/cache/cache.hh"

namespace tsugumi::cache {

// A connection whose peer the caller has already authenticated: the TLS
// stream of the listener, or a byte stream relayed for a worker that cannot
// reach the listener (the relay carries the worker's own TLS session, so the
// listener still authenticates the worker itself).
class Connection {
 public:
  virtual ~Connection() = default;
  // Some bytes, or empty when the peer closed.
  virtual auto ReadSome(size_t limit) -> std::string = 0;
  virtual auto Write(std::string_view bytes) -> void = 0;
};

// Serves HTTP/1.1 requests until the peer closes or a request cannot be
// read: GET/HEAD of `nix-cache-info`, `.narinfo`, `.ls` and `nar/` files,
// and PUT of `.narinfo`, `.ls` and `nar/` files, as `nix copy --to` sends
// them. `peer` names the authenticated key in the log.
auto Serve(Cache& cache, Connection& connection, std::string_view peer) -> void;

}  // namespace tsugumi::cache
