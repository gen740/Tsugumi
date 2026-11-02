#include "lanes.hh"

#include "util/log.hh"

namespace tsugumi {

Lane::Lane(size_t threads, size_t capacity, std::chrono::milliseconds wait_limit)
    : capacity_(capacity), wait_limit_(wait_limit) {
  for (size_t i = 0; i < threads; ++i) threads_.emplace_back([this] { run(); });
}

Lane::~Lane() { stop(); }

void Lane::stop() {
  {
    std::lock_guard<std::mutex> l(mu_);
    if (stopping_) return;
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto& t : threads_)
    if (t.joinable()) t.join();
}

size_t Lane::queued() const {
  std::lock_guard<std::mutex> l(mu_);
  return q_.size();
}

bool Lane::submit(Task t) {
  {
    std::lock_guard<std::mutex> l(mu_);
    if (stopping_ || q_.size() >= capacity_) return false;
    q_.push_back({std::chrono::steady_clock::now(), std::move(t)});
  }
  cv_.notify_one();
  return true;
}

void Lane::run() {
  while (true) {
    Item item;
    {
      std::unique_lock<std::mutex> l(mu_);
      cv_.wait(l, [&] { return stopping_ || !q_.empty(); });
      if (stopping_ && q_.empty()) return;
      item = std::move(q_.front());
      q_.pop_front();
    }
    bool expired = std::chrono::steady_clock::now() - item.enqueued > wait_limit_;
    try {
      item.task(expired);
    } catch (const std::exception& e) {
      log::error(std::string("lane task escaped: ") + e.what());
    }
  }
}

}  // namespace tsugumi
