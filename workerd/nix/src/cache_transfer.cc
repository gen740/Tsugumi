#include "tsugumi/workerd/cache_transfer.hh"

#include <algorithm>
#include <functional>
#include <map>
#include <nix/store/derived-path.hh>
#include <nix/store/globals.hh>
#include <nix/store/keys.hh>
#include <nix/store/path-info.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <set>
#include <stdexcept>

namespace tsugumi {
namespace {

using Info = nix::ref<const nix::ValidPathInfo>;

// The paths of `wanted` that a configured substituter serves with metadata
// this machine trusts, as Nix would substitute them.
auto served_by_pulls(const nix::StoreDirConfig& store, nix::StorePathSet wanted)
    -> std::map<nix::StorePath, Info> {
  std::map<nix::StorePath, Info> served;
  if (wanted.empty()) return served;
  const auto keys = nix::getDefaultPublicKeys();
  for (const auto& pull : nix::getDefaultSubstituters()) {
    if (wanted.empty()) break;
    nix::StorePathSet found;
    try {
      found = pull->queryValidPaths(wanted);
    } catch (const std::exception&) {
      continue;  // An unreachable pull cache serves nothing.
    }
    for (const auto& path : found) {
      try {
        auto info = pull->queryPathInfo(path);
        if (!pull->config.isTrusted && nix::settings.requireSigs &&
            !info->isContentAddressed(store) &&
            info->checkSignatures(store, keys) == 0)
          continue;
        served.emplace(path, std::move(info));
        wanted.erase(path);
      } catch (const std::exception&) {
      }
    }
  }
  return served;
}

// References first: `refs` gives each path's references within the set.
auto topological(const std::map<nix::StorePath, nix::StorePathSet>& refs)
    -> std::vector<nix::StorePath> {
  std::vector<nix::StorePath> order;
  std::set<nix::StorePath> done;
  std::set<nix::StorePath> visiting;
  std::function<void(const nix::StorePath&)> visit =
      [&](const nix::StorePath& path) {
        if (done.contains(path) || visiting.contains(path)) return;
        visiting.insert(path);
        for (const auto& ref : refs.at(path))
          if (ref != path && refs.contains(ref)) visit(ref);
        visiting.erase(path);
        done.insert(path);
        order.push_back(path);
      };
  for (const auto& [path, _] : refs) visit(path);
  return order;
}

}  // namespace

auto plan_cache_push(nix::Store& source, const std::string& root,
                     nix::Store* target) -> CachePushPlan {
  nix::StorePathSet closure;
  source.computeFSClosure(source.parseStorePath(root), closure);
  CachePushPlan plan;
  const auto served = served_by_pulls(source, closure);
  plan.served = served.size();
  nix::StorePathSet wanted;
  for (const auto& path : closure)
    if (!served.contains(path)) wanted.insert(path);
  if (target && !wanted.empty()) {
    const auto present = target->queryValidPaths(wanted);
    plan.present = present.size();
    for (const auto& path : present) wanted.erase(path);
  }
  for (const auto& path : source.topoSortPaths(wanted)) {
    plan.bytes += source.queryPathInfo(path)->narSize;
    plan.paths.push_back(source.printStorePath(path));
  }
  // topoSortPaths puts referrers first.
  std::reverse(plan.paths.begin(), plan.paths.end());
  return plan;
}

auto fetch_closure(nix::Store& store, const std::string& root, nix::Store* own,
                   const CacheTransferReport& report) -> bool {
  const auto start = store.parseStorePath(root);
  // Walk the missing part of the closure, asking the pull caches first.
  std::map<nix::StorePath, Info> pulled;
  std::map<nix::StorePath, Info> relayed;
  std::map<nix::StorePath, nix::StorePathSet> refs;
  nix::StorePathSet frontier{start};
  while (!frontier.empty()) {
    nix::StorePathSet missing;
    const auto valid = store.queryValidPaths(frontier);
    for (const auto& path : frontier)
      if (!valid.contains(path) && !refs.contains(path)) missing.insert(path);
    nix::StorePathSet next;
    auto found = served_by_pulls(store, missing);
    for (auto& [path, info] : found) {
      missing.erase(path);
      refs[path] = info->references;
      next.insert(info->references.begin(), info->references.end());
      pulled.emplace(path, std::move(info));
    }
    if (own)
      for (const auto& path : missing) {
        if (!own->isValidPath(path)) continue;
        auto info = own->queryPathInfo(path);
        refs[path] = info->references;
        next.insert(info->references.begin(), info->references.end());
        relayed.emplace(path, std::move(info));
      }
    for (const auto& path : refs) next.erase(path.first);
    frontier = std::move(next);
  }
  CacheTransferProgress progress{0, refs.size(), 0, 0, {}};
  for (const auto& [_, info] : pulled) progress.bytes_total += info->narSize;
  for (const auto& [_, info] : relayed) progress.bytes_total += info->narSize;
  if (refs.empty()) return false;
  if (report) report(progress);
  // A pull cache holds the closures of its paths, so Nix substitutes them
  // together, in parallel. Only if one references a path that the built-in
  // cache alone has does this fall back to one path at a time.
  if (!pulled.empty()) {
    std::vector<nix::DerivedPath> paths;
    for (const auto& [path, _] : pulled)
      paths.push_back(nix::DerivedPath::Opaque{path});
    try {
      store.buildPaths(paths);
      for (const auto& [_, info] : pulled) {
        ++progress.done;
        progress.bytes_done += info->narSize;
      }
      if (report) report(progress);
    } catch (const std::exception&) {
      if (relayed.empty()) throw;
    }
  }
  for (const auto& path : topological(refs)) {
    if (store.isValidPath(path)) continue;
    progress.path = store.printStorePath(path);
    if (report) report(progress);
    if (const auto from = relayed.find(path); from != relayed.end()) {
      nix::copyStorePath(*own, store, path, nix::NoRepair, nix::CheckSigs);
      progress.bytes_done += from->second->narSize;
    } else {
      // Its references are valid by now, so Nix substitutes only this path.
      store.ensurePath(path);
      progress.bytes_done += pulled.at(path)->narSize;
    }
    ++progress.done;
  }
  progress.path.clear();
  progress.done = progress.total;
  progress.bytes_done = progress.bytes_total;
  if (report) report(progress);
  return relayed.contains(start);
}

}  // namespace tsugumi
