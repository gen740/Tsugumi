#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "tsugumi/workerd/cache_relay_store.hh"
#include "tsugumi/workerd/derivation.hh"
#include "tsugumi/workerd/protocol.hh"
#include "tsugumi/workerd/store_result.hh"

namespace tsugumi {
// What realise reports while it runs, for the job's status.
struct StoreProgress {
  // Another invocation holds the store; this one waits for it.
  std::function<void()> waiting;
  // This invocation now holds the store.
  std::function<void()> locked;
  // A derivation build starts (true) or ends (false), by its .drv path.
  std::function<void(const std::string&, bool)> build;
  // Nix starts substituting this store path.
  std::function<void(const std::string&)> substitute;
};

// Store operations only: remote workers never construct a NixEvaluator.
class RemoteWorkerStore {
 public:
  static auto realise(const DerivationClosureTransport& transport,
                      const Tsugumi::RemoteWorker::CacheSettingsT& caches,
                      CacheRelay::Exchange relay,
                      const std::filesystem::path& store_root,
                      const WorkerDerivation& derivation, bool push_outputs,
                      const std::function<void(const std::string&)>& log,
                      uint64_t timeout_ms = 0,
                      const StoreProgress& progress = {}) -> WorkerStoreResult;
};

}  // namespace tsugumi
