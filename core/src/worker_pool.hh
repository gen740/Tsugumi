// Native workers share the host store; their outputs are realised before spawn.
// The project's Card workers: resident ones that stay across runs, and
// standby ones started ahead for Cards that take a fresh worker per call.
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "worker_proc.hh"

namespace tsugumi {

struct WorkerLimits {
  std::chrono::seconds idle{600};            // a resident or standby worker unused this long ends
  uint64_t max_rss_bytes = 2ull << 30;       // larger workers end instead of staying
  size_t max_resident = 32;                  // the least recently used idle ones end first
  size_t max_standby = 32;                   // idle and pending fresh workers
};

class WorkerPool {
 public:
  WorkerPool(std::filesystem::path scratch_root, WorkerLimits limits = {});
  ~WorkerPool();  // ends every worker (the project stops)

  struct Lease {
    std::unique_ptr<WorkerProc> worker;
    bool reused = false;  // a resident worker that served an earlier call
  };

  // A worker for `card` running `key` (build and execution conditions; the
  // spawn spec without its scratch folder): lease an idle resident worker
  // only when reuse is enabled. Otherwise take a fresh standby or start one.
  // Residents of this Card end when reuse is disabled or the key changes.
  Lease acquire(const std::string& card, const std::string& key, const WorkerSpawn& spawn, bool reuse);
  // Gives the worker back after a call. A clean call of a reusing Card keeps
  // it resident; anything else ends it.
  void release(const std::string& card, const std::string& key, std::unique_ptr<WorkerProc> worker, bool keep);
  // Ends every worker of the project (definitions reloaded, cache cleared).
  void clear();

  size_t resident_count();
  size_t standby_count();

 private:
  struct Idle {
    std::string card;
    std::string key;
    std::unique_ptr<WorkerProc> worker;
    std::chrono::steady_clock::time_point since;
  };
  std::unique_ptr<WorkerProc> spawn(WorkerSpawn spawn);
  void refill(const std::string& key, WorkerSpawn spawn);
  void reaper();

  std::filesystem::path root_;
  WorkerLimits limits_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<Idle> resident_;
  std::map<std::string, Idle> standby_;  // one per key
  std::map<std::string, bool> refilling_;
  std::deque<std::pair<std::string, WorkerSpawn>> pending_;  // standby workers to start
  std::deque<std::packaged_task<std::unique_ptr<WorkerProc>()>> spawns_;  // workers a call waits for
  uint64_t seq_ = 0;
  bool stopping_ = false;
  std::thread reaper_;  // starts standby workers, ends idle ones
};

}  // namespace tsugumi
