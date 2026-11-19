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
  std::string manifest;  // its contents
  std::vector<std::string>
      closure;         // runtime references, including the manifest
  bool built = false;  // false: the result was already in the store
};

}  // namespace tsugumi
