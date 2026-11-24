// Execution lanes. The mutation lane serializes every revision check (saves,
// reloads, imports, run acceptance, Open); the read pool serves snapshots and
// database reads so that slow work never blocks the transport loop.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace tsugumi {

class Lane {
 public:
  using Task = std::function<void()>;
  explicit Lane(size_t threads);
  ~Lane();
  // Returns false after shutdown.
  bool submit(Task t);
  void stop();
  auto idle() const -> bool;
  auto pending() const -> size_t;

 private:
  void run();
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Task> q_;
  bool stopping_ = false;
  size_t running_ = 0;
  std::vector<std::thread> threads_;
};

}  // namespace tsugumi
