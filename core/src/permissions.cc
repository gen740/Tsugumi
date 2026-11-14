#include "permissions.hh"

#include <set>

#include "document.hh"

namespace tsugumi::permissions {

std::vector<std::string> widenings(const Resolution* before, const Resolution& after) {
  std::vector<std::string> out;
  std::set<std::string> sources;
  if (before)
    for (const auto& n : before->nodes)
      for (const auto& s : n.nix_sources) sources.insert(s.ref + "\n" + s.overrides);
  for (const auto& n : after.nodes)
    for (const auto& s : n.nix_sources)
      if (sources.insert(s.ref + "\n" + s.overrides).second)
        out.push_back("nix_source " + s.name + " " + s.ref +
                      (s.overrides == "{}" ? "" : " overrides " + s.overrides));
  for (const auto& n : after.nodes) {
    auto key = path_key(n.path);
    const Node* b = before ? before->find(n.path) : nullptr;
    const auto& s = n.settings;
    if (s.execution_service != kLocalExecutionService && (!b || b->settings.execution_service != s.execution_service))
      out.push_back("execution_service " + key + " " + s.execution_service);
    if (s.network && !(b && b->settings.network)) out.push_back("network " + key);
    if (!s.sandbox && (!b || b->settings.sandbox)) out.push_back("sandbox_off " + key);
    for (const auto& f : s.host_folders) {
      const HostFolder* had = nullptr;
      if (b)
        for (const auto& g : b->settings.host_folders)
          if (g.path == f.path) had = &g;
      if (!had) out.push_back(std::string(f.write ? "host_folder_write " : "host_folder_read ") + key + " " + f.path);
      else if (f.write && !had->write) out.push_back("host_folder_write " + key + " " + f.path);
    }
  }
  return out;
}

}  // namespace tsugumi::permissions
