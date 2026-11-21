#include "permissions.hh"

#include <set>

#include "tsugumi/document/document.hh"

namespace tsugumi::permissions {

std::vector<std::string> widenings(const Resolution* before,
                                   const Resolution& after) {
  std::vector<std::string> out;
  std::set<std::string> sources;
  if (before)
    for (const auto& n : before->nodes)
      for (const auto& s : n.nix_sources)
        sources.insert(s.ref + "\n" + s.overrides);
  for (const auto& n : after.nodes)
    for (const auto& s : n.nix_sources)
      if (sources.insert(s.ref + "\n" + s.overrides).second)
        out.push_back("nix_source " + s.name + " " + s.ref +
                      (s.overrides == "{}" ? "" : " overrides " + s.overrides));
  for (const auto& n : after.nodes) {
    auto key = path_key(n.path);
    const Node* b = before ? before->find(n.path) : nullptr;
    const auto& s = n.settings;
    if (s.execution_service != kLocalExecutionService &&
        s.execution_service != "auto" &&
        (!b || b->settings.execution_service != s.execution_service))
      out.push_back("execution_service " + key + " " + s.execution_service);
    if (s.grants.network && !(b && b->settings.grants.network))
      out.push_back("network " + key);
    if (s.grants.gui && !(b && b->settings.grants.gui))
      out.push_back("gui " + key);
    const SandboxOptions defaults;
    const auto& prior = b ? b->settings.sandbox : defaults;
    if (!s.sandbox.filesystem && prior.filesystem)
      out.push_back("sandbox_off " + key);
    auto relaxed = [](uint64_t next, uint64_t old) {
      return old && (!next || next > old);
    };
    if ((!s.sandbox.pid && prior.pid) || (!s.sandbox.ipc && prior.ipc) ||
        (!s.sandbox.identity && prior.identity) ||
        relaxed(s.sandbox.limits.cpu_millis, prior.limits.cpu_millis) ||
        relaxed(s.sandbox.limits.memory_bytes, prior.limits.memory_bytes))
      out.push_back("sandbox_options " + key);
    for (const auto& f : s.grants.host_paths) {
      const HostFolder* had = nullptr;
      if (b)
        for (const auto& g : b->settings.grants.host_paths)
          if (g.path == f.path) had = &g;
      if (!had)
        out.push_back(
            std::string(f.write ? "host_folder_write " : "host_folder_read ") +
            key + " " + f.path);
      else if (f.write && !had->write)
        out.push_back("host_folder_write " + key + " " + f.path);
    }
  }
  return out;
}

}  // namespace tsugumi::permissions
