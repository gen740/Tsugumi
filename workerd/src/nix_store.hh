#pragma once

#include "worker_derivation.hh"

namespace tsugumi {
// Store operations only: remote workers never construct a NixEvaluator.
class RemoteWorkerStore {
 public:
  static auto realise(const DerivationClosureTransport& transport,
                      const std::filesystem::path& store_root,
                      const WorkerDerivation& derivation,
                      const std::function<void(const std::string&)>& log)
      -> NixResult;
};

}  // namespace tsugumi
