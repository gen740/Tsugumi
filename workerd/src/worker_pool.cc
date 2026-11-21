#include "tsugumi/workerd/worker_pool.hh"

#include <algorithm>
#include <future>

#include "tsugumi/util/log.hh"

namespace tsugumi {

namespace stdfs = std::filesystem;

WorkerPool::WorkerPool(stdfs::path scratch_root, WorkerLimits limits,
                       WorkerPoolMode mode)
    : root_(std::move(scratch_root)), limits_(limits), mode_(mode) {
  if (mode_ == WorkerPoolMode::one_shot) return;
  std::error_code ec;
  stdfs::remove_all(root_, ec);  // left by an earlier tsugumid of this project
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
  // One-shot scratch belongs to its worker (or, if construction failed, to
  // the existing job/supervisor cleanup). Do not add a second cleanup owner.
  if (mode_ == WorkerPoolMode::one_shot) return;
  std::error_code ec;
  stdfs::remove_all(root_, ec);
}

// Workers are started on the pool's own thread: bubblewrap's
// --die-with-parent ends a worker when the *thread* that started it ends, and
// a run's lanes end with the run while its workers stay resident.
std::unique_ptr<WorkerProc> WorkerPool::spawn(WorkerSpawn sp) {
  if (mode_ == WorkerPoolMode::one_shot) {
    sp.scratch = root_.string();
    return std::make_unique<WorkerProc>(sp);
  }
  {
    std::lock_guard<std::mutex> l(mu_);
    stdfs::create_directories(root_);
    stdfs::permissions(root_, stdfs::perms::owner_all);
    sp.scratch = (root_ / ("w" + std::to_string(++seq_))).string();
  }
  if (std::this_thread::get_id() == reaper_.get_id())
    return std::make_unique<WorkerProc>(sp);
  std::packaged_task<std::unique_ptr<WorkerProc>()> task(
      [sp] { return std::make_unique<WorkerProc>(sp); });
  auto result = task.get_future();
  {
    std::lock_guard<std::mutex> l(mu_);
    if (stopping_)
      throw Failed(FailureKind::worker_failed, "the project is stopping");
    spawns_.push_back(std::move(task));
  }
  cv_.notify_all();
  return result.get();
}

WorkerPool::Lease WorkerPool::acquire(const std::string& card,
                                      const std::string& key,
                                      const WorkerSpawn& sp, bool reuse) {
  std::string standby_key = key;
  Lease lease;
  std::vector<std::unique_ptr<WorkerProc>> ended;
  {
    std::lock_guard<std::mutex> l(mu_);
    if (reuse) {
      if (active_residents_.contains(card))
        throw Failed(FailureKind::busy,
                     "the Card's resident is already in use");
      lease.generation = ++lease_sequence_;
      active_residents_[card] = lease.generation;
      if (auto previous = resident_keys_.find(card);
          previous != resident_keys_.end()) {
        lease.replaced = previous->second == key
                             ? "the resident worker died"
                             : "the Card definition changed";
      }
      if (auto reason = resident_replacements_.find(card);
          reason != resident_replacements_.end()) {
        if (lease.replaced.empty() ||
            lease.replaced == "the resident worker died")
          lease.replaced = reason->second;
        resident_replacements_.erase(reason);
      }
      resident_keys_[card] = key;
    } else {
      resident_keys_.erase(card);
      resident_replacements_.erase(card);
    }
    for (auto it = resident_.begin(); it != resident_.end();) {
      if (it->card != card) {
        ++it;
        continue;
      }
      if (reuse && it->key == key && it->worker->alive() && !lease.worker) {
        lease.worker = std::move(it->worker);
        lease.reused = true;
        lease.replaced.clear();
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
        if (it->second.worker->alive())
          lease.worker = std::move(it->second.worker);
        else
          ended.push_back(std::move(it->second.worker));
        standby_.erase(it);
      }
    }
  }
  ended.clear();  // outside the lock: ending a worker waits for it
  if (!lease.worker) {
    try {
      lease.worker = spawn(sp);
    } catch (...) {
      if (lease.generation) {
        std::lock_guard guard(mu_);
        active_residents_.erase(card);
      }
      throw;
    }
  }
  if (!reuse && !sp.secrets && mode_ == WorkerPoolMode::standby)
    refill(standby_key, sp);
  return lease;
}

void WorkerPool::release(const std::string& card, uint64_t generation,
                         std::unique_ptr<WorkerProc> worker, bool keep,
                         const std::string& reason) {
  if (!worker || !generation) return;
  worker->set_logs(nullptr);
  std::lock_guard guard(mu_);
  const auto active = active_residents_.find(card);
  if (active == active_residents_.end() || active->second != generation) return;
  active_residents_.erase(active);
  const auto key = resident_keys_.find(card);
  // Invalidation retired this version while the worker was on loan.
  if (key == resident_keys_.end()) return;
  if (keep && worker->alive()) {
    resident_.push_back({card, key->second, std::move(worker)});
  } else if (!reason.empty()) {
    resident_replacements_[card] = reason;
  }
}

auto WorkerPool::invalidate(const std::string& card, const std::string& reason)
    -> void {
  const auto prefix = card + "|";
  const auto belongs = [&](const std::string& id) {
    return id == card || id.starts_with(prefix);
  };
  std::vector<std::unique_ptr<WorkerProc>> ended;
  std::lock_guard guard(mu_);
  for (auto it = resident_keys_.begin(); it != resident_keys_.end();) {
    if (!belongs(it->first)) {
      ++it;
      continue;
    }
    resident_replacements_[it->first] = reason;
    it = resident_keys_.erase(it);
  }
  std::erase_if(resident_, [&](Idle& idle) {
    if (!belongs(idle.card)) return false;
    ended.push_back(std::move(idle.worker));
    return true;
  });
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
  std::lock_guard guard(mu_);
  for (auto& r : resident_) ended.push_back(std::move(r.worker));
  resident_.clear();
  resident_keys_.clear();
  resident_replacements_.clear();
  for (auto& [_, standby] : standby_)
    ended.push_back(std::move(standby.worker));
  standby_.clear();
  ++standby_epoch_;
  pending_.clear();
  refilling_.clear();
}

void WorkerPool::reaper() {
  std::unique_lock<std::mutex> l(mu_);
  while (!stopping_) {
    cv_.wait_for(l, std::chrono::seconds(30), [&] {
      return stopping_ || !pending_.empty() || !spawns_.empty();
    });
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
      const auto epoch = standby_epoch_;
      l.unlock();
      std::unique_ptr<WorkerProc> w;
      try {
        w = spawn(sp);
      } catch (const std::exception& e) {
        log::warn(std::string("cannot start a standby worker: ") + e.what());
      }
      l.lock();
      if (epoch == standby_epoch_) refilling_.erase(key);
      if (w && epoch == standby_epoch_ && !stopping_ && !standby_.count(key))
        standby_[key] = {"", key, std::move(w)};
      else if (w) {
        l.unlock();
        w.reset();
        l.lock();
      }
    }
    // Release dead workers and bound retained standby memory.
    std::vector<std::unique_ptr<WorkerProc>> ended;
    for (auto it = standby_.begin(); it != standby_.end();)
      if (auto rss = it->second.worker->rss_bytes();
          !it->second.worker->alive() ||
          (rss && *rss > limits_.max_rss_bytes)) {
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
