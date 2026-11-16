#include "host/layout_saving.hh"

#include <algorithm>
#include <map>
#include <set>

#include "agent/json.hh"
#include "host/layout.hh"
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

auto CompleteLayout(const std::string& project, const std::string& text,
                    const LayoutRead& read) -> std::string {
  ValidateLayout(text);
  auto doc = json::Object(text);
  auto positions = json::Object(json::Get(doc, "cards", "{}"));
  const auto graph =
      json::Object(json::Get(json::Object(project), "canvas", "{}"));
  layout::Scene scene;
  scene.top_to_bottom = json::Text(doc, "flow", "topToBottom") == "topToBottom";
  std::vector<std::string> missing;
  auto number = [](const json::Members& value, const std::string& key,
                   double fallback) {
    return value.contains(key) ? std::stod(value.at(key)) : fallback;
  };
  const auto displays = json::Object(json::Get(doc, "displays", "{}"));
  const auto geometry = DefaultDisplayGeometry(scene.top_to_bottom);
  const auto other_geometry = DefaultDisplayGeometry(!scene.top_to_bottom);
  for (const auto& raw : json::Array(json::Get(graph, "cards", "[]"))) {
    const auto id = json::Text(json::Object(raw), "id");
    const auto position = json::Object(json::Get(positions, id, "{}"));
    if (!positions.contains(id)) missing.push_back(id);
    // A host reservation is independent of a window's zoom and grid. Leave
    // room for the normal expanded Card; Arrange uses measured UI geometry.
    size_t params = 0, ports = 0;
    if (read)
      if (const auto file = read("definitions/" + id + "/_card.json")) {
        const auto definition = json::Object(file->text);
        params = json::Array(json::Get(definition, "params", "[]")).size();
        ports = json::Array(json::Get(definition, "inputs", "[]")).size();
      }
    std::set<std::string> incoming, outgoing;
    for (const auto& raw_wire :
         json::Array(json::Get(graph, "connections", "[]"))) {
      const auto wire = json::Object(raw_wire);
      for (const auto* side : {"from", "to"}) {
        const auto end = json::Object(json::Get(wire, side, "{}"));
        if (json::Text(end, "card") == id)
          (std::string_view(side) == "from" ? outgoing : incoming)
              .insert(json::Text(end, "port"));
      }
    }
    ports = std::max({ports, incoming.size(), outgoing.size()});
    const double width =
        std::max(320.0, 64.0 * static_cast<double>(ports) + 24);
    const double height = 240 + 26.0 * static_cast<double>(params) +
                          30.0 * static_cast<double>(ports);
    // A default frame follows the measured Card edge in each UI. Our Card
    // reservation bounds normal UI sizes, so reserve the whole envelope from
    // the Card origin through that frame, including smaller/folded Cards.
    // Reserve it before any run too: placement must not depend on run history.
    const bool default_frame = !displays.contains(id);
    auto reserve = [&](const auto& d, bool horizontal) {
      return horizontal ? d.anchor_x * width + d.x + d.width
                        : d.anchor_y * height + d.y + d.height;
    };
    // Without a saved direction, windows may infer a flow or use their own
    // preference. Reserve both possible default frames in that case.
    const double reserved_width =
        default_frame
            ? std::max(
                  {width, reserve(geometry, true),
                   doc.contains("flow") ? 0 : reserve(other_geometry, true)})
            : width;
    const double reserved_height =
        default_frame
            ? std::max(
                  {height, reserve(geometry, false),
                   doc.contains("flow") ? 0 : reserve(other_geometry, false)})
            : height;
    scene.cards.push_back({id,
                           number(position, "x", 0),
                           number(position, "y", 0),
                           reserved_width,
                           reserved_height,
                           {},
                           {},
                           false});
  }
  if (missing.empty()) return text;
  for (const auto& raw : json::Array(json::Get(graph, "connections", "[]"))) {
    const auto w = json::Object(raw);
    const auto from = json::Object(json::Get(w, "from", "{}"));
    const auto to = json::Object(json::Get(w, "to", "{}"));
    scene.wires.push_back({json::Text(from, "card"), json::Text(from, "port"),
                           json::Text(to, "card"), json::Text(to, "port")});
  }
  for (const auto& [id, raw] : json::Object(json::Get(doc, "displays", "{}"))) {
    const auto b = json::Object(raw);
    if (!json::Bool(json::Get(b, "hidden")))
      scene.frames.push_back({id, number(b, "x", 0), number(b, "y", 0),
                              number(b, "width", 320),
                              number(b, "height", 240)});
  }
  for (const auto& raw : json::Array(json::Get(doc, "notes", "[]"))) {
    const auto b = json::Object(raw);
    scene.notes.push_back({json::Text(b, "id"), number(b, "x", 0),
                           number(b, "y", 0), number(b, "width", 240),
                           number(b, "height", 160)});
  }
  for (const auto& p : layout::place_missing(scene, missing).cards)
    positions[p.id] = json::Encode(
        json::Members{{"x", std::to_string(p.x)}, {"y", std::to_string(p.y)}});
  doc["cards"] = json::Encode(positions);
  return json::Encode(doc);
}

auto PrepareLayoutSave(const std::string& arguments, const LayoutRead& read,
                       const std::vector<std::string> &documents)
    -> std::string {
  auto args = json::Object(arguments);
  auto changes = json::Array(json::Get(args, "changes", "[]"));
  struct Edit {
    size_t index;
    std::string text;
    bool source;
  };
  std::map<std::string, Edit> projects, layouts;
  std::map<std::string, LayoutFile> pending;
  std::set<std::string> deleted_projects;
  for (size_t i = 0; i < changes.size(); ++i) {
    const auto change = json::Object(changes[i]);
    const auto type = json::Text(change, "value_type");
    const auto value = json::Object(json::Get(change, "value", "{}"));
    if (type == "PutDefinition" && !json::Text(value, "card_json").empty())
      pending["definitions/" + json::Text(value, "id") + "/_card.json"] = {
          json::Text(value, "card_json"), {}};
    if (json::Text(value, "path").ends_with("/_card.json")) {
      const auto path = json::Text(value, "path");
      if (type == "PutSource")
        pending[path] = {Bytes(json::Get(value, "bytes")), {}};
      else if (type == "PatchSource") {
        if (const auto file = read(path))
          pending[path] = {Patched(*file, value), {}};
      }
    }
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
      if (project)
        deleted_projects.insert(prefix);
      if (layout)
        layouts[prefix] = {i, "{}", true};
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
  for (const auto& prefix : deleted_projects) layouts.erase(prefix);
  // Existing unplaced Cards are materialized by the next save of any kind,
  // including a source-only edit, at every external document depth.
  for (const auto &path : documents) {
    const auto prefix = path.substr(0, path.size() - 12);
    if (deleted_projects.contains(prefix) || projects.contains(prefix))
      continue;
    if (const auto file = read(path))
      projects[prefix] = {0, file->text, !prefix.empty()};
  }
  for (const auto& [prefix, project] : projects) {
    if (deleted_projects.contains(prefix))
      continue;
    const auto previous = read(prefix + "tsugumi.json");
    const auto before =
        previous ? CardIds(previous->text) : std::set<std::string>{};
    const auto after = CardIds(project.text);
    std::set<std::string> removed;
    for (const auto& id : before)
      if (!after.contains(id)) removed.insert(id);
    const auto file = read(prefix + "layout.json");
    const auto old = layouts.contains(prefix) ? layouts.at(prefix).text
                     : file                   ? file->text
                                              : "{}";
    auto seeded = ValidateLayout(old, removed);
    if (previous) {
      const auto stored = file ? file->text : "{}";
      const auto derived = json::Object(
          CompleteLayout(previous->text, stored, [&](const std::string &path) {
            return read(prefix + path);
          }));
      const auto stored_positions =
          json::Object(json::Get(json::Object(stored), "cards", "{}"));
      auto doc = json::Object(seeded);
      auto positions = json::Object(json::Get(doc, "cards", "{}"));
      bool changed = false;
      for (const auto &[id, position] :
           json::Object(json::Get(derived, "cards", "{}"))) {
        if (after.contains(id) && !stored_positions.contains(id) &&
            !positions.contains(id)) {
          positions[id] = position;
          changed = true;
        }
      }
      if (changed) {
        doc["cards"] = json::Encode(positions);
        seeded = json::Encode(doc);
      }
    }
    const auto completed = CompleteLayout(
        project.text, seeded,
        [&](const std::string &path) -> std::optional<LayoutFile> {
          const auto file = prefix + path;
          if (pending.contains(file))
            return pending.at(file);
          return read(file);
        });
    if (layouts.contains(prefix)) {
      layouts.at(prefix).text = completed;
    } else if (completed != old) {
      const auto i = changes.size();
      const auto value =
          project.source
              ? json::Members{{"path", json::Quote(prefix + "layout.json")},
                              {"bytes", json::Quote(completed)},
                              {"base_hash", file && file->hash
                                                ? json::Quote(*file->hash)
                                                : "null"}}
              : json::Members{{"json", json::Quote(completed)}};
      changes.push_back(json::Encode(json::Members{
          {"value_type",
           json::Quote(project.source ? "PutSource" : "SetLayout")},
          {"value", json::Encode(value)}}));
      layouts[prefix] = {i, completed, project.source};
    }
  }
  for (const auto& [prefix, layout] : layouts) {
    ValidateLayout(layout.text);
    auto change = json::Object(changes.at(layout.index));
    auto value = json::Object(json::Get(change, "value"));
    if (json::Text(change, "value_type") == "RemoveSource" && layout.text == "{}") continue;
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
