#include "host/layout_saving.hh"

#include <map>
#include <set>

#include "agent/json.hh"
#include "host/layout_service.hh"
#include "util/crypto.hh"

namespace tsugumi::host {
namespace {
namespace json = agent::json;
auto Bytes(const std::string& raw) -> std::string {
  if (!raw.empty() && raw.front() == '"') return json::String(raw);
  std::string text;
  if (!raw.empty() && raw.front() == '{') {
    if (!crypto::base64_decode(json::Text(json::Object(raw), "base64"), text))
      throw LayoutFailure("invalid layout source bytes");
  } else {
    for (const auto& byte : json::Array(raw)) {
      const auto n = json::UInt(byte, 256);
      if (n > 255) throw LayoutFailure("invalid layout source byte");
      text += static_cast<char>(n);
    }
  }
  return text;
}
auto Integer(const std::string& raw) -> uint64_t {
  return json::UInt(raw.front() == '"' ? json::String(raw) : raw, UINT64_MAX);
}
auto Patched(const LayoutFile& file, const json::Members& value)
    -> std::string {
  if (!file.hash || *file.hash != json::Text(value, "base_hash"))
    throw LayoutFailure("layout source hash changed");
  std::string out;
  size_t cursor = 0;
  const auto patches = json::Array(json::Get(value, "patches", "[]"));
  if (patches.empty())
    throw LayoutFailure("layout source patches must not be empty");
  for (const auto& raw : patches) {
    const auto patch = json::Object(raw);
    const auto offset = Integer(json::Get(patch, "offset"));
    const auto remove = Integer(json::Get(patch, "remove"));
    if (offset < cursor || offset > file.text.size() ||
        remove > file.text.size() - offset)
      throw LayoutFailure("invalid layout source patch range");
    auto boundary = [&](size_t at) {
      return at == file.text.size() ||
             (static_cast<unsigned char>(file.text[at]) & 0xc0) != 0x80;
    };
    if (!boundary(offset) || !boundary(offset + remove))
      throw LayoutFailure("layout source patch splits UTF-8");
    out.append(file.text, cursor, offset - cursor);
    out += json::Text(patch, "text");
    cursor = offset + remove;
  }
  out.append(file.text, cursor, std::string::npos);
  return out;
}
auto CardIds(const std::string& text) -> std::set<std::string> {
  std::set<std::string> out;
  const auto graph =
      json::Object(json::Get(json::Object(text), "canvas", "{}"));
  for (const auto& card : json::Array(json::Get(graph, "cards", "[]")))
    out.insert(json::Text(json::Object(card), "id"));
  return out;
}
}  // namespace

auto IsExternalDocument(std::string_view path, std::string_view name) -> bool {
  bool external = false;
  while (path.starts_with("externals/")) {
    const auto slash = path.find('/', 10);
    if (slash == std::string_view::npos) return false;
    path.remove_prefix(slash + 1);
    external = true;
  }
  return external && path == name;
}

auto PrepareLayoutSave(const std::string& arguments, const LayoutRead& read)
    -> std::string {
  auto args = json::Object(arguments);
  auto changes = json::Array(json::Get(args, "changes", "[]"));
  struct Edit {
    size_t index;
    std::string text;
    bool source;
  };
  std::map<std::string, Edit> projects, layouts;
  std::set<std::string> deleted_layouts;
  for (size_t i = 0; i < changes.size(); ++i) {
    const auto change = json::Object(changes[i]);
    const auto type = json::Text(change, "value_type");
    const auto value = json::Object(json::Get(change, "value", "{}"));
    if (type == "SetProject")
      projects[""] = {i, json::Text(value, "json"), false};
    if (type == "SetLayout")
      layouts[""] = {i, json::Text(value, "json"), false};
    const auto path = json::Text(value, "path");
    const bool layout = IsExternalDocument(path, "layout.json");
    const bool project = IsExternalDocument(path, "tsugumi.json");
    if (!layout && !project) continue;
    const auto prefix = path.substr(0, path.size() - (layout ? 11 : 12));
    if (type == "RemoveSource") {
      if (layout) deleted_layouts.insert(prefix);
      continue;
    }
    if (type != "PutSource" && type != "PatchSource") continue;
    std::string text;
    if (type == "PutSource")
      text = Bytes(json::Get(value, "bytes"));
    else {
      const auto file = read(path);
      if (!file) throw LayoutFailure("missing layout source for patch");
      text = Patched(*file, value);
    }
    (layout ? layouts : projects)[prefix] = {i, std::move(text), true};
  }
  for (const auto& [prefix, project] : projects) {
    const auto previous = read(prefix + "tsugumi.json");
    if (!previous || deleted_layouts.contains(prefix)) continue;
    const auto before = CardIds(previous->text), after = CardIds(project.text);
    std::set<std::string> removed;
    for (const auto& id : before)
      if (!after.contains(id)) removed.insert(id);
    if (removed.empty()) continue;
    if (!layouts.contains(prefix)) {
      const auto file = read(prefix + "layout.json");
      if (!file) continue;
      const auto i = changes.size();
      const auto value =
          project.source
              ? json::Members{{"path", json::Quote(prefix + "layout.json")},
                              {"bytes", json::Quote(file->text)},
                              {"base_hash",
                               file->hash ? json::Quote(*file->hash) : "null"}}
              : json::Members{{"json", json::Quote(file->text)}};
      changes.push_back(json::Encode(json::Members{
          {"value_type",
           json::Quote(project.source ? "PutSource" : "SetLayout")},
          {"value", json::Encode(value)}}));
      layouts[prefix] = {i, file->text, project.source};
    }
    auto& layout = layouts.at(prefix);
    layout.text = ValidateLayout(layout.text, removed);
  }
  for (const auto& [prefix, layout] : layouts) {
    ValidateLayout(layout.text);
    auto change = json::Object(changes.at(layout.index));
    auto value = json::Object(json::Get(change, "value"));
    value[layout.source ? "bytes" : "json"] = json::Quote(layout.text);
    if (layout.source) {
      change["value_type"] = json::Quote("PutSource");
      value.erase("patches");
    }
    change["value"] = json::Encode(value);
    changes[layout.index] = json::Encode(change);
  }
  args["changes"] = json::Encode(changes);
  return json::Encode(args);
}
}  // namespace tsugumi::host
