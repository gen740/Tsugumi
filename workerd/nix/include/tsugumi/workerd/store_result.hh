// Results of preparing a definition in a Nix store.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "tsugumi/workerd/derivation.hh"
namespace tsugumi {
struct WorkerStoreResult {
  WorkerDerivation derivation;
  std::string plan;  // the Card build plan JSON
  std::vector<std::string>
      closure;         // runtime references, including the build plan
  bool built = false;
  std::string substituter;
};

// What publishing a build's outputs to the push target did, after the Card ran.
struct CachePushOutcome {
  bool pushed = false;   // the target accepted every path it lacked
  uint64_t paths = 0;    // paths uploaded
  uint64_t bytes = 0;    // their NAR size
  uint64_t skipped = 0;  // closure paths a pull cache or the target had
  std::string error;     // why the upload failed
};

}  // namespace tsugumi
