#include "tsugumi/workerd/store.hh"
#include <mutex>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
namespace tsugumi {
auto NixStore::init_process() -> void {
  static std::once_flag initialized;
  std::call_once(initialized, [] { nix::initLibStore(true); });
}
std::vector<std::string> NixStore::substituters() {
  std::vector<std::string> urls;
  for (const auto& ref : nix::settings.getWorkerSettings().substituters.get()) urls.push_back(ref.render(false));
  return urls;
}

auto NixStore::rooted_usage(const std::vector<std::vector<std::string>>& roots)
    -> std::vector<std::map<std::string, uint64_t>> {
  auto store = nix::openStore();
  std::vector<std::map<std::string, uint64_t>> result;
  std::map<std::string, uint64_t> sizes;
  for (const auto& project : roots) {
    nix::StorePathSet closure;
    for (const auto& path : project)
      store->computeFSClosure(store->parseStorePath(path), closure, false,
                              nix::settings.getLocalSettings().keepOutputs,
                              nix::settings.getLocalSettings().keepDerivations);
    auto& measured = result.emplace_back();
    for (const auto& path : closure) {
      auto name = store->printStorePath(path);
      auto [it, inserted] = sizes.try_emplace(name, 0);
      if (inserted) it->second = store->queryPathInfo(path)->narSize;
      measured.emplace(name, it->second);
    }
  }
  return result;
}

}  // namespace tsugumi
