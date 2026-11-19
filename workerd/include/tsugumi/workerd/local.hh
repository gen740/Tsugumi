#pragma once

#include <array>
#include <filesystem>
#include <mutex>

#include "tsugumi/protocol/local_worker_generated.h"
#include "tsugumi/workerd/worker_pool.hh"

namespace tsugumi::workerd {

// Descriptors are closed unless explicitly taken by the receiver.
struct LocalPacket {
  std::unique_ptr<Tsugumi::LocalWorker::MessageT> message;
  std::array<int, 4> descriptors{-1, -1, -1, -1};
  ~LocalPacket();
};
auto send_local(int fd, const Tsugumi::LocalWorker::MessageT& message,
                const std::array<int, 4>* descriptors = nullptr,
                trace::Link* link = nullptr) -> void;
auto receive_local(int fd, LocalPacket& packet, trace::Link* link = nullptr) -> bool;
auto serve_local(int fd, const std::filesystem::path& scratch) -> void;

// One workerd process for this core lifetime, connected by an inherited socket.
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

 private:
  auto exchange(Tsugumi::LocalWorker::MessageT message, LocalPacket& reply)
      -> void;
  auto command(Tsugumi::LocalWorker::Action action, uint64_t lease,
               bool force = false) -> void;
  std::filesystem::path binary_;
  std::filesystem::path scratch_;
  std::mutex mutex_;
  int fd_ = -1;
  pid_t pid_ = -1;
};

}  // namespace tsugumi::workerd
