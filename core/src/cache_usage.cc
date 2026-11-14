#include "cache_usage.hh"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>

#include "nix_eval.hh"
#include "run_scratch.hh"
#include "util/fs.hh"
#include "util/log.hh"

namespace tsugumi {
namespace stdfs = std::filesystem;
namespace R = Tsugumi::RPC;

CacheUsage::CacheUsage(stdfs::path roots, stdfs::path scratch, stdfs::path single_roots,
                       stdfs::path single_scratch, std::string single_id, stdfs::path publish)
    : roots_(std::move(roots)),
      scratch_(std::move(scratch)),
      single_roots_(std::move(single_roots)),
      single_scratch_(std::move(single_scratch)),
      publish_(std::move(publish)),
      single_id_(std::move(single_id)) {
  result_.error = "Measuring cache usage";
  thread_ = std::thread([this] { loop(); });
}

CacheUsage::~CacheUsage() {
  {
    std::lock_guard lock(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  thread_.join();
}

auto CacheUsage::snapshot() -> R::ProjectCacheUsageResultT {
  std::lock_guard lock(mu_);
  return result_;
}

auto CacheUsage::refresh() -> void {
  {
    std::lock_guard lock(mu_);
    refresh_ = true;
  }
  cv_.notify_all();
}

auto CacheUsage::measure() -> R::ProjectCacheUsageResultT {
  R::ProjectCacheUsageResultT result;
  std::set<std::string> ids;
  std::error_code error;
  if (!single_id_.empty())
    ids.insert(single_id_);
  else
    for (const auto& base : {roots_, scratch_}) {
      if (!stdfs::exists(base)) continue;
      for (const auto& entry : stdfs::directory_iterator(base))
        if (stdfs::is_directory(entry.symlink_status())) ids.insert(entry.path().filename().string());
    }
  std::vector<std::vector<std::string>> roots;
  for (const auto& id : ids) {
    const auto folder = single_id_.empty() ? roots_ / id : single_roots_;
    const auto scratch = single_id_.empty() ? scratch_ / id : single_scratch_;
    auto project = std::make_unique<R::ProjectCacheUsageT>();
    project->project = id;
    project->name = id;
    try {
      std::string label;
      fs::stream_beneath(folder, ".name", 4096, [&](std::string_view chunk) { label.append(chunk); });
      if (!label.empty()) project->name = std::move(label);
    } catch (const std::exception&) { /* A missing label does not hide usage. */
    }
    struct stat st{};
    int64_t last = 0;
    const auto used = [&](const stdfs::path& path) {
      if (::lstat(path.c_str(), &st) == 0) last = std::max(last, static_cast<int64_t>(st.st_mtime) * 1000);
    };
    used(folder);
    auto& paths = roots.emplace_back();
    if (stdfs::is_directory(folder))
      for (const auto& root : stdfs::directory_iterator(folder)) {
        if (!stdfs::is_symlink(root.symlink_status())) continue;
        used(root.path());
        auto target = stdfs::read_symlink(root.path());
        if (target.is_relative()) target = root.path().parent_path() / target;
        paths.push_back(target.lexically_normal().string());
      }
    if (last && !paths.empty()) project->last_used = last;
    try {
      uint64_t bytes = 0;
      for (const auto& entry : measure_run_scratch(scratch)) {
        auto item = std::make_unique<R::RunScratchUsageT>();
        item->path = entry.path;
        item->bytes = entry.bytes;
        bytes += entry.bytes;
        project->run_scratch.push_back(std::move(item));
      }
      project->scratch_bytes = bytes;
    } catch (const std::exception& e) {
      project->run_scratch.clear();
      project->error = e.what();
    }
    result.projects.push_back(std::move(project));
  }
  try {
    const auto closures = NixEvaluator::rooted_usage(roots);
    std::map<std::string, uint64_t> all;
    for (size_t i = 0; i < closures.size(); ++i) {
      uint64_t bytes = 0;
      for (const auto& [path, size] : closures[i]) {
        bytes += size;
        all.emplace(path, size);
      }
      result.projects[i]->build_bytes = bytes;
    }
    uint64_t total = 0;
    for (const auto& [path, size] : all) total += size;
    result.build_bytes = total;
  } catch (const std::exception& e) {
    result.error = e.what();
  }
  const auto disk = stdfs::space("/nix/store", error);
  if (!error) {
    result.store_capacity_bytes = disk.capacity;
    result.store_available_bytes = disk.available;
    result.threshold_bytes = cache_notice_threshold(disk.capacity);
    result.large = result.build_bytes && cache_is_large(*result.build_bytes, disk.capacity);
  } else if (result.error.empty())
    result.error = "Store disk usage unavailable: " + error.message();
  uint64_t scratch_bytes = 0;
  for (const auto& project : result.projects)
    scratch_bytes += project->scratch_bytes.value_or(0);
  const auto scratch_disk = stdfs::space(single_id_.empty() ? scratch_ : single_scratch_, error);
  if (!error) result.large = result.large || cache_is_large(scratch_bytes, scratch_disk.capacity);
  result.measured_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  return result;
}

auto CacheUsage::loop() -> void {
  std::unique_lock lock(mu_);
  while (!stopping_) {
    refresh_ = false;
    lock.unlock();
    R::ProjectCacheUsageResultT next;
    try {
      next = measure();
    } catch (const std::exception& e) {
      next.error = e.what();
    }
    if (!publish_.empty()) {
      try {
        flatbuffers::FlatBufferBuilder builder;
        builder.Finish(R::ProjectCacheUsageResult::Pack(builder, &next));
        fs::write_atomic(
            publish_, {reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize()}, 0444);
      } catch (const std::exception& e) {
        log::warn(std::string("cache usage: ") + e.what());
      }
    }
    lock.lock();
    result_ = std::move(next);
    cv_.wait_for(lock, std::chrono::seconds(60), [this] { return stopping_ || refresh_; });
  }
}

}  // namespace tsugumi
