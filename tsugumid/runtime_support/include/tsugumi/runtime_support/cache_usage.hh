#pragma once

#include <filesystem>
#include <mutex>

#include "tsugumi/protocol/tsugumi_generated.h"

namespace tsugumi {

// Measure only when Settings requests usage.
class CacheUsage {
 public:
  CacheUsage(std::filesystem::path roots,
             std::filesystem::path single_roots = {},
             std::string single_id = {}, std::filesystem::path publish = {});
  auto snapshot() -> Tsugumi::RPC::ProjectCacheUsageResultT;

 private:
  auto measure() -> Tsugumi::RPC::ProjectCacheUsageResultT;
  std::filesystem::path roots_, single_roots_, publish_;
  std::string single_id_;
  std::mutex mu_;
};

}  // namespace tsugumi
