#include "worker_pool.hh"

#include <algorithm>
#include <future>

#include "util/log.hh"

namespace tsugumi {

namespace stdfs = std::filesystem;

WorkerPool::WorkerPool(stdfs::path scratch_root, WorkerLimits limits) : root_(std::move(scratch_root)), limits_(limits) {
  std::error_code ec;
  stdfs::remove_all(root_, ec);  // left by an earlier tsugumid of this project
  stdfs::create_directories(root_);
  stdfs::permissions(root_, stdfs::perms::owner_all, ec);
  reaper_ = std::thread([this] { reaper(); });
}

WorkerPool::~WorkerPool() {
  {
    std::lock_guard<std::mutex> l(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (reaper_.joinable()) reaper_.join();
  spawns_.clear();  // anyone still waiting gets broken_promise
  clear();
  std::error_code ec;
  stdfs::remove_all(root_, ec);
}

// Workers are started on the pool's own thread: bubblewrap's
// --die-with-parent ends a worker when the *thread* that started it ends, and
// a run's lanes end with the run while its workers stay resident.
std::unique_ptr<WorkerProc> WorkerPool::spawn(WorkerSpawn sp) {
  {
    std::lock_guard<std::mutex> l(mu_);
    sp.scratch = (root_ / ("w" + std::to_string(++seq_))).string();
  }
  if (std::this_thread::get_id() == reaper_.get_id()) return std::make_unique<WorkerProc>(sp);
  std::packaged_task<std::unique_ptr<WorkerProc>()> task([sp] { return std::make_unique<WorkerProc>(sp); });
  auto result = task.get_future();
  {
    std::lock_guard<std::mutex> l(mu_);
    if (stopping_) throw Failed(FailureKind::worker_failed, "the project is stopping");
    spawns_.push_back(std::move(task));
  }
  cv_.notify_all();
  return result.get();
}

WorkerPool::Lease WorkerPool::acquire(const std::string& card, const std::string& key, const WorkerSpawn& sp,
                                      bool reuse) {
  std::string standby_key = key;
#ifdef __APPLE__
  // Seatbelt grants each worker write access to this Card's outputs.
  if (sp.sandbox) standby_key += "|" + card;
#endif
  Lease lease;
  std::vector<std::unique_ptr<WorkerProc>> ended;
  {
    std::lock_guard<std::mutex> l(mu_);
    for (auto it = resident_.begin(); it != resident_.end();) {
      if (it->card != card) {
        ++it;
        continue;
      }
      if (reuse && it->key == key && it->worker->alive() && !lease.worker) {
        lease.worker = std::move(it->worker);
        lease.reused = true;
      } else if (!reuse || it->key != key || !it->worker->alive()) {
        // Reuse was disabled, the build changed, or the worker died idle.
        ended.push_back(std::move(it->worker));
      } else {
        ++it;
        continue;
      }
      it = resident_.erase(it);
    }
    if (!lease.worker && !reuse) {
      if (auto it = standby_.find(standby_key); it != standby_.end()) {
        if (it->second.worker->alive()) lease.worker = std::move(it->second.worker);
        else ended.push_back(std::move(it->second.worker));
        standby_.erase(it);
      }
    }
  }
  ended.clear();  // outside the lock: ending a worker waits for it
  if (!lease.worker) lease.worker = spawn(sp);
  if (!reuse) refill(standby_key, sp);
  return lease;
}

void WorkerPool::release(const std::string& card, const std::string& key, std::unique_ptr<WorkerProc> worker,
                         bool keep) {
  if (!worker) return;
  worker->set_logs(nullptr);
  if (keep && worker->alive()) {
    if (auto rss = worker->rss_bytes(); rss && *rss > limits_.max_rss_bytes) {
      log::info("ending a resident worker of " + card + " above the memory limit (" + std::to_string(*rss >> 20) + " MiB)");
      keep = false;
    }
  } else {
    keep = false;
  }
  if (!keep) return;  // worker ends here
  std::vector<std::unique_ptr<WorkerProc>> ended;
  {
    std::lock_guard<std::mutex> l(mu_);
    // One idle worker per Card and key; the least recently used ones beyond
    // the limit end.
    for (auto it = resident_.begin(); it != resident_.end();)
      if (it->card == card && it->key == key) {
        ended.push_back(std::move(it->worker));
        it = resident_.erase(it);
      } else {
        ++it;
      }
    resident_.push_back({card, key, std::move(worker), std::chrono::steady_clock::now()});
    while (resident_.size() > limits_.max_resident) {
      auto oldest = std::min_element(resident_.begin(), resident_.end(),
                                     [](const Idle& a, const Idle& b) { return a.since < b.since; });
      ended.push_back(std::move(oldest->worker));
      resident_.erase(oldest);
    }
  }
}

void WorkerPool::refill(const std::string& key, WorkerSpawn sp) {
  {
    std::lock_guard<std::mutex> l(mu_);
    if (stopping_ || standby_.contains(key) || refilling_.contains(key) ||
        standby_.size() + refilling_.size() >= limits_.max_standby)
      return;
    refilling_[key] = true;
    pending_.emplace_back(key, std::move(sp));
  }
  cv_.notify_all();
}

void WorkerPool::clear() {
  std::vector<std::unique_ptr<WorkerProc>> ended;
  std::lock_guard<std::mutex> l(mu_);
  for (auto& r : resident_) ended.push_back(std::move(r.worker));
  for (auto& [_, s] : standby_) ended.push_back(std::move(s.worker));
  resident_.clear();
  standby_.clear();
}

void WorkerPool::reaper() {
  std::unique_lock<std::mutex> l(mu_);
  while (!stopping_) {
    cv_.wait_for(l, std::chrono::seconds(30), [&] { return stopping_ || !pending_.empty() || !spawns_.empty(); });
    if (stopping_) break;
    // Workers a call waits for come first.
    while (!spawns_.empty()) {
      auto task = std::move(spawns_.front());
      spawns_.pop_front();
      l.unlock();
      task();
      l.lock();
    }
    // Standby workers, one at a time (spawned outside the lock).
    while (!pending_.empty() && !stopping_ && spawns_.empty()) {
      auto [key, sp] = std::move(pending_.front());
      pending_.pop_front();
      l.unlock();
      std::unique_ptr<WorkerProc> w;
      try {
        w = spawn(sp);
      } catch (const std::exception& e) {
        log::warn(std::string("cannot start a standby worker: ") + e.what());
      }
      l.lock();
      refilling_.erase(key);
      if (w && !stopping_ && !standby_.count(key)) standby_[key] = {"", key, std::move(w), std::chrono::steady_clock::now()};
      else if (w) {
        l.unlock();
        w.reset();
        l.lock();
      }
    }
    // Idle workers past the limit end.
    std::vector<std::unique_ptr<WorkerProc>> ended;
    auto now = std::chrono::steady_clock::now();
    for (auto it = resident_.begin(); it != resident_.end();)
      if (now - it->since > limits_.idle) {
        ended.push_back(std::move(it->worker));
        it = resident_.erase(it);
      } else {
        ++it;
      }
    for (auto it = standby_.begin(); it != standby_.end();)
      if (now - it->second.since > limits_.idle) {
        ended.push_back(std::move(it->second.worker));
        it = standby_.erase(it);
      } else {
        ++it;
      }
    l.unlock();
    ended.clear();
    l.lock();
  }
}

}  // namespace tsugumi
