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
  bool pushed = false;
};

}  // namespace tsugumi
