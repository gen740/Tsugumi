#include "tsugumi/runtime_support/cache_usage.hh"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>

#include "tsugumi/util/fs.hh"
#include "tsugumi/util/log.hh"
#include "tsugumi/workerd/store.hh"

namespace tsugumi {
namespace stdfs = std::filesystem;
namespace R = Tsugumi::RPC;

CacheUsage::CacheUsage(stdfs::path roots, stdfs::path single_roots,
                       std::string single_id, stdfs::path publish)
    : roots_(std::move(roots)),
      single_roots_(std::move(single_roots)),
      publish_(std::move(publish)),
      single_id_(std::move(single_id)) {}

auto CacheUsage::snapshot() -> R::ProjectCacheUsageResultT {
  std::lock_guard lock(mu_);
  auto result = measure();
  if (!publish_.empty()) {
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(R::ProjectCacheUsageResult::Pack(builder, &result));
    fs::write_atomic(publish_,
                     {reinterpret_cast<const char*>(builder.GetBufferPointer()),
                      builder.GetSize()},
                     0444);
  }
  return result;
}

auto CacheUsage::measure() -> R::ProjectCacheUsageResultT {
  R::ProjectCacheUsageResultT result;
  std::set<std::string> ids;
  std::error_code error;
  if (!single_id_.empty())
    ids.insert(single_id_);
  else if (stdfs::exists(roots_))
    for (const auto& entry : stdfs::directory_iterator(roots_))
      if (stdfs::is_directory(entry.symlink_status()))
        ids.insert(entry.path().filename().string());
  std::vector<std::vector<std::string>> roots;
  for (const auto& id : ids) {
    const auto folder = single_id_.empty() ? roots_ / id : single_roots_;
    auto project = std::make_unique<R::ProjectCacheUsageT>();
    project->project = id;
    project->name = id;
    try {
      std::string label;
      fs::stream_beneath(folder, ".name", 4096,
                         [&](std::string_view chunk) { label.append(chunk); });
      if (!label.empty()) project->name = std::move(label);
    } catch (const std::exception&) { /* A missing label does not hide usage. */
    }
    struct stat st{};
    int64_t last = 0;
    const auto used = [&](const stdfs::path& path) {
      if (::lstat(path.c_str(), &st) == 0)
        last = std::max(last, static_cast<int64_t>(st.st_mtime) * 1000);
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
    result.projects.push_back(std::move(project));
  }
  try {
    const auto closures = NixStore::rooted_usage(roots);
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
  } else if (result.error.empty())
    result.error = "Store disk usage unavailable: " + error.message();
  result.measured_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  return result;
}

}  // namespace tsugumi
