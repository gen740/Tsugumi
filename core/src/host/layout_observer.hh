#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "host/registry.hh"

namespace tsugumi::host {
// A host subscription fills missing places after any writer/reload changes a
// workflow. The project only transports layout bytes and performs CAS saves.
class LayoutObserver {
 public:
  using Call = std::function<std::string(const std::string&, const std::string&,
                                         const std::string&)>;
  explicit LayoutObserver(Call call);
  ~LayoutObserver();
  auto Watch(const std::string& project,
             const ProjectRegistry::Project& endpoint) -> void;
  auto Stop() -> void;

 private:
  struct Watcher;
  Call call_;
  std::mutex mutex_;
  bool stopped_ = false;
  std::vector<std::unique_ptr<Watcher>> retired_;
  std::map<std::string, std::unique_ptr<Watcher>> watchers_;
};
}  // namespace tsugumi::host
