#include "permissions.hh"

#include "document.hh"

namespace tsugumi::permissions {

std::vector<std::string> widenings(const Resolution* before, const Resolution& after) {
  std::vector<std::string> out;
  for (const auto& n : after.nodes) {
    auto key = path_key(n.path);
    const Node* b = before ? before->find(n.path) : nullptr;
    const auto& s = n.settings;
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
