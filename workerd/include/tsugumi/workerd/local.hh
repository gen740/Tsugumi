#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <span>

#include "tsugumi/protocol/local_worker_generated.h"
#include "tsugumi/workerd/worker_pool.hh"

namespace tsugumi::workerd {

// Descriptors are closed unless explicitly taken by the receiver.
struct LocalPacket {
  std::unique_ptr<Tsugumi::LocalWorker::MessageT> message;
  std::array<int, 4> descriptors{-1, -1, -1, -1};
  ~LocalPacket();
};
auto clear_spawn_secrets(Tsugumi::LocalWorker::MessageT& message) -> void;
auto send_local(int fd, const Tsugumi::LocalWorker::MessageT& message,
                const std::array<int, 4>* descriptors = nullptr,
                trace::Link* link = nullptr) -> void;
auto receive_local(int fd, LocalPacket& packet, trace::Link* link = nullptr)
    -> bool;
// The first connection owns resident leases and pool-wide commands. The
// remaining connections let cold invocations acquire and retire in parallel.
inline constexpr size_t kLocalWorkerChannels = 5;
auto serve_local(std::span<const int> fds, const std::filesystem::path& scratch)
    -> void;

// One workerd process for this core lifetime, with inherited sockets.
// The core owns only borrowed worker channels; it never starts a Card process.
class LocalWorkers {
 public:
  LocalWorkers(std::filesystem::path binary, std::filesystem::path scratch);
  ~LocalWorkers();
  auto acquire(const std::string& card, const std::string& key,
               const WorkerSpawn& spawn, bool reuse) -> WorkerPool::Lease;
  auto release(std::unique_ptr<WorkerProc> worker, bool keep,
               const std::string& reason) -> void;
  auto clear() -> void;
  auto invalidate(const std::string& card, const std::string& reason) -> void;

 private:
  auto connect() -> void;
  auto started() -> bool;
  auto exchange(size_t channel, Tsugumi::LocalWorker::MessageT message,
                LocalPacket& reply) -> void;
  auto command(size_t channel, Tsugumi::LocalWorker::Action action,
               uint64_t lease, bool force = false) -> void;
  std::filesystem::path binary_;
  std::filesystem::path scratch_;
  struct Channel {
    std::mutex mutex;
    int fd = -1;
  };
  std::array<Channel, kLocalWorkerChannels> channels_;
  std::mutex start_mutex_;
  std::atomic<size_t> next_fresh_channel_{0};
  // Serialize lease transitions with reader ownership. workerd still owns
  // eviction and termination. Retired sockets are removed after commands.
  std::mutex leases_mutex_;
  std::map<std::string, std::unique_ptr<WorkerProc>> idle_;
  pid_t pid_ = -1;
};

}  // namespace tsugumi::workerd
