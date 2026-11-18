#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

#include "tsugumi/protocol/tsugumi_generated.h"

namespace tsugumi {

// Measure only when Settings requests usage.
class CacheUsage {
 public:
  CacheUsage(std::filesystem::path roots, std::filesystem::path scratch,
             std::filesystem::path single_roots = {},
             std::filesystem::path single_scratch = {},
             std::string single_id = {}, std::filesystem::path publish = {});
  auto snapshot() -> Tsugumi::RPC::ProjectCacheUsageResultT;

 private:
  auto measure() -> Tsugumi::RPC::ProjectCacheUsageResultT;
  std::filesystem::path roots_, scratch_, single_roots_, single_scratch_,
      publish_;
  std::string single_id_;
  std::mutex mu_;
};

}  // namespace tsugumi
