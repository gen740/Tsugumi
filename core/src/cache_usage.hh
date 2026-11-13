#pragma once

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

#include "tsugumi/protocol/tsugumi_generated.h"

namespace tsugumi {

constexpr auto cache_notice_threshold(uint64_t capacity) -> uint64_t {
  return std::max(uint64_t{1} << 30, capacity / 5);
}
constexpr auto cache_is_large(uint64_t bytes, uint64_t capacity) -> bool {
  return capacity != 0 && bytes >= cache_notice_threshold(capacity);
}

// One background measurement per minute. The VM owns one shared instance;
// host daemons measure the launcher's registry. Requests only copy a snapshot.
class CacheUsage {
 public:
  CacheUsage(std::filesystem::path roots, std::filesystem::path scratch,
             std::filesystem::path single_roots = {}, std::filesystem::path single_scratch = {},
             std::string single_id = {}, std::filesystem::path publish = {});
  ~CacheUsage();
  auto snapshot() -> Tsugumi::RPC::ProjectCacheUsageResultT;
  auto refresh() -> void;

 private:
  auto measure() -> Tsugumi::RPC::ProjectCacheUsageResultT;
  auto loop() -> void;
  std::filesystem::path roots_, scratch_, single_roots_, single_scratch_, publish_;
  std::string single_id_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stopping_ = false;
  bool refresh_ = true;
  Tsugumi::RPC::ProjectCacheUsageResultT result_;
  std::thread thread_;
};

}  // namespace tsugumi
