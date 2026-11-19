#pragma once

#include <cstdint>

#include "tsugumi/workerd/derivation.hh"
#include "tsugumi/workerd/store_result.hh"

namespace tsugumi {
// Store operations only: remote workers never construct a NixEvaluator.
class RemoteWorkerStore {
 public:
  static auto realise(const DerivationClosureTransport& transport,
                      const std::filesystem::path& store_root,
                      const WorkerDerivation& derivation,
                      const std::function<void(const std::string&)>& log,
                      uint64_t timeout_ms = 0) -> WorkerStoreResult;
};

}  // namespace tsugumi
