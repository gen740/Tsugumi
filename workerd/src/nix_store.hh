#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "tsugumi/workerd/cache_relay_store.hh"
#include "tsugumi/workerd/cache_transfer.hh"
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
  // Fetching the derivation's or output's missing closure: from a pull cache,
  // or the built-in cache for what no pull cache serves.
  CacheTransferReport fetch;
};

// Store operations only: remote workers never construct a NixEvaluator.
class RemoteWorkerStore {
 public:
  static auto realise(const DerivationClosureTransport& transport,
                      const Tsugumi::RemoteWorker::CacheSettingsT& caches,
                      CacheRelay::Exchange relay,
                      const std::filesystem::path& store_root,
                      const WorkerDerivation& derivation,
                      const std::function<void(const std::string&)>& log,
                      uint64_t timeout_ms = 0,
                      const StoreProgress& progress = {}) -> WorkerStoreResult;
  // Uploads the part of the output's closure that neither a pull cache nor
  // the target has. Called after the Card ran, in the process that realised
  // the derivation; it never throws, and a failure is in the outcome.
  static auto push(const Tsugumi::RemoteWorker::CacheSettingsT& caches,
                   CacheRelay::Exchange relay,
                   const std::filesystem::path& store_root,
                   const WorkerDerivation& derivation,
                   const CacheTransferReport& report) -> CachePushOutcome;
};

}  // namespace tsugumi
