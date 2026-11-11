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

namespace {

// A Card's _meta.json: a document at the root, a source inside a Card in Card.
// One that cannot be read counts as empty.
schema::CardMeta meta_of(const Document& doc, const SourceReader& read, const Node& n) {
  if (n.project_dir.empty()) {
    auto it = doc.definitions.find(n.path.back());
    if (it != doc.definitions.end() && it->second.meta) return *it->second.meta;
    return {};
  }
  try {
    if (auto text = read(n.def_dir + "_meta.json")) return parse_meta_json(*text);
  } catch (const DocumentError&) {
  }
  return {};
}

template <class F>
std::string text_of(const F& field) {
  const auto* v = schema::get(field);
  return v ? std::string(*v) : std::string();
}

bool blank(const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }

}  // namespace

std::vector<std::string> incomplete_draft_intents(const Document& before, const SourceReader& read_before,
                                                  const Document& after, const SourceReader& read_after) {
  std::vector<std::string> out;
  if (!before.resolution || !after.resolution) return out;
  for (const auto& n : after.resolution->nodes) {
    if (n.kind == Template::Draft) continue;
    const Node* was = before.resolution->find(n.path);
    if (!was || was->kind != Template::Draft) continue;

    auto key = path_key(n.path);
    auto old_meta = meta_of(before, read_before, *was);
    auto new_meta = meta_of(after, read_after, n);
    auto old_description = text_of(old_meta.description), old_content = text_of(old_meta.content);
    auto old_context = text_of(old_meta.context);
    auto keep = [&](const char* name, const std::string& was_text, const std::string& now) {
      if (!blank(was_text) && now != was_text)
        out.push_back(key + ": " + name + " changed; send what the Draft had, word for word");
    };
    keep("description", old_description, text_of(new_meta.description));
    keep("content", old_content, text_of(new_meta.content));
    keep("context", old_context, text_of(new_meta.context));
    if (blank(text_of(new_meta.description)))
      out.push_back(key + ": description is empty; write a one-line summary of what the Card does");
    if (blank(text_of(new_meta.context)) && !(blank(old_description) && blank(old_content)))
      out.push_back(key + ": context is empty; write what the user asked of this Card and what its description and "
                          "content say a later reader should know, inventing nothing");
  }
  return out;
}

}  // namespace tsugumi::permissions
