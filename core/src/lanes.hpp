// Execution lanes. The mutation lane serializes every revision check (saves,
// reloads, imports, run acceptance, Open); the read pool serves snapshots and
// database reads so that slow work never blocks the transport loop.
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace tsugumi {

class Lane {
 public:
  using Task = std::function<void(bool expired)>;
  Lane(size_t threads, size_t capacity, std::chrono::milliseconds wait_limit);
  ~Lane();
  // Returns false when the queue is full (caller replies `busy`). A task that
  // waited longer than wait_limit runs with expired=true and must reply
  // `timeout` without doing the work.
  bool submit(Task t);
  void stop();
  size_t queued() const;

 private:
  struct Item {
    std::chrono::steady_clock::time_point enqueued;
    Task task;
  };
  void run();
  size_t capacity_;
  std::chrono::milliseconds wait_limit_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Item> q_;
  bool stopping_ = false;
  std::vector<std::thread> threads_;
};

}  // namespace tsugumi
