#include "host/layout_saving.hh"

#include "host/layout_file.hh"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "host/layout.hh"
#include "host/layout_service.hh"
#include "host/save.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/text.hh"

namespace tsugumi::host {
namespace {
namespace json = ::tsugumi::json;
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
auto Patched(const LayoutFile& file, const json::Members& value,
             const SourcePatches& patches) -> std::string {
  if (!file.hash || *file.hash != json::Text(value, "base_hash"))
    throw LayoutFailure("layout source hash changed");
  const auto raw = json::Array(json::Get(value, "patches", "[]"));
  if (raw.empty())
    throw LayoutFailure("layout source patches must not be empty");
  try {
    return patches.Project(file.text, raw);
  } catch (const text::PatchRangeError& error) {
    throw LayoutFailure(error.what());
  }
}
auto CardIds(const std::string& text, const std::string& path)
    -> std::set<std::string> {
  std::set<std::string> out;
  const auto graph = json::Object(
      json::Get(json::Object(text, path), "canvas", "{}"), path + ".canvas");
  size_t index = 0;
  for (const auto& card :
       json::Array(json::Get(graph, "cards", "[]"), path + ".canvas.cards"))
    out.insert(json::Text(json::Object(card, path + ".canvas.cards[" +
                                                 std::to_string(index++) + "]"),
                          "id"));
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

static auto TransformLayout(
    const std::string& project, const std::string& text, const LayoutRead& read,
    int action, const Tsugumi::Host::LayoutSceneT* measurements = nullptr,
    const Tsugumi::Host::ProjectMoveT* move = nullptr) -> std::string {
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
  for (const auto& raw : json::Array(json::Get(graph, "cards", "[]"))) {
    const auto id = json::Text(json::Object(raw), "id");
    const auto position = json::Object(json::Get(positions, id, "{}"));
    if (!position.contains("x") || !position.contains("y"))
      missing.push_back(id);
    // A host reservation is independent of a window's zoom and grid. Leave
    // room for the normal expanded Card; Arrange uses measured UI geometry.
    size_t params = 0, ports = 0;
    if (read)
      if (const auto file = read("definitions/" + id + "/_card.json")) {
        const auto definition =
            json::Object(file->text, "definitions/" + id + "/_card.json");
        params = json::Object(json::Get(definition, "params", "{}"),
                              "definitions/" + id + "/_card.json.params")
                     .size();
        ports = json::Array(json::Get(definition, "inputs", "[]"),
                            "definitions/" + id + "/_card.json.inputs")
                    .size();
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
    scene.cards.push_back({id,
                           number(position, "x", 0),
                           number(position, "y", 0),
                           width,
                           height,
                           {},
                           {},
                           action >= 0});
    if (action >= 0) {
      auto add_ports = [&](const auto& names, auto& destination, bool output) {
        size_t n = 0;
        for (const auto& name : names) {
          const auto along = 40.0 + 64.0 * static_cast<double>(n++);
          destination.push_back(
              {name, scene.top_to_bottom ? along : (output ? width : 0),
               scene.top_to_bottom ? (output ? height : 0) : along});
        }
      };
      add_ports(incoming, scene.cards.back().inputs, false);
      add_ports(outgoing, scene.cards.back().outputs, true);
    }
  }
  if (missing.empty() && action < 0) return text;
  for (const auto& raw : json::Array(json::Get(graph, "connections", "[]"))) {
    const auto w = json::Object(raw);
    const auto from = json::Object(json::Get(w, "from", "{}"));
    const auto to = json::Object(json::Get(w, "to", "{}"));
    scene.wires.push_back({json::Text(from, "card"), json::Text(from, "port"),
                           json::Text(to, "card"), json::Text(to, "port")});
  }
  for (const auto& raw : json::Array(json::Get(doc, "notes", "[]"))) {
    const auto b = json::Object(raw);
    scene.notes.push_back({json::Text(b, "id"), number(b, "x", 0),
                           number(b, "y", 0), number(b, "width", 240),
                           number(b, "height", 160)});
  }
  if (action == 1) {
    // Moves and port sorting use the same positions as the displayed canvas.
    for (const auto& placed : layout::place_missing(scene, missing).cards)
      for (auto& card : scene.cards)
        if (card.id == placed.id) {
          card.x = placed.x;
          card.y = placed.y;
          break;
        }
  }
  if (measurements) {
    const auto measured = MeasuredScene(*measurements);
    if (!doc.contains("flow")) scene.top_to_bottom = measured.top_to_bottom;
    // Flow may have changed while the request travelled. Current-document
    // geometry remains the fallback for new Cards and stale measurements.
    if (measured.top_to_bottom == scene.top_to_bottom) {
      scene.grid = measured.grid;
      scene.snap = measured.snap;
      for (auto& card : scene.cards) {
        const auto found = std::find_if(
            measured.cards.begin(), measured.cards.end(),
            [&](const auto& value) { return value.id == card.id; });
        if (found == measured.cards.end()) continue;
        auto covers = [](const auto& required, const auto& ports) {
          return std::all_of(
              required.begin(), required.end(), [&](const auto& port) {
                return std::any_of(
                    ports.begin(), ports.end(),
                    [&](const auto& p) { return p.name == port.name; });
              });
        };
        if (!covers(card.inputs, found->inputs) ||
            !covers(card.outputs, found->outputs))
          continue;
        const auto x = card.x, y = card.y;
        card = *found;
        card.x = x;
        card.y = y;
        auto saved_order = [&](auto& ports, const std::string& key) {
          if (!positions.contains(card.id)) return;
          const auto entry = json::Object(positions.at(card.id));
          if (!entry.contains(key)) return;
          std::vector<std::string> names;
          for (const auto& raw : json::Array(entry.at(key))) {
            const auto name = json::String(raw);
            if (std::find(names.begin(), names.end(), name) == names.end() &&
                std::any_of(ports.begin(), ports.end(),
                            [&](const auto& p) { return p.name == name; }))
              names.push_back(name);
          }
          for (const auto& port : ports)
            if (std::find(names.begin(), names.end(), port.name) == names.end())
              names.push_back(port.name);
          for (size_t i = 0; i < ports.size(); ++i) ports[i].name = names[i];
        };
        saved_order(card.inputs, "inputOrder");
        saved_order(card.outputs, "outputOrder");
      }
      for (auto& note : scene.notes)
        for (const auto& size : measured.notes)
          if (size.id == note.id) {
            note.width = size.width;
            note.height = size.height;
          }
    }
    const auto boundary =
        json::Object(json::Get(json::Object(project), "boundary", "{}"));
    const auto places = json::Object(json::Get(doc, "boundary", "{}"));
    auto bar = [&](const std::string& side, auto& destination) {
      for (const auto& raw : json::Array(json::Get(boundary, side, "[]"))) {
        const auto end = json::Object(raw);
        destination.ends.push_back({json::Text(end, "name"),
                                    json::Text(end, "card"),
                                    json::Text(end, "port")});
      }
      if (places.contains(side)) {
        const auto at = json::Object(places.at(side));
        destination.at = layout::Point{number(at, "x", 0), number(at, "y", 0)};
      }
    };
    bar("inputs", scene.inputs);
    bar("outputs", scene.outputs);
  }
  if (move) {
    if (!std::isfinite(move->dx) || !std::isfinite(move->dy) ||
        !std::isfinite(move->snap) || move->snap < 0)
      throw LayoutFailure("move coordinates and grid must be finite");
    auto has = [](const auto& ids, const std::string& id) {
      return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    auto snap = [&](double value) {
      return move->snap > 0 ? std::floor(value / move->snap + 0.5) * move->snap
                            : value;
    };
    const auto dx = move->cards.empty() ? move->dx : snap(move->dx);
    const auto dy = move->cards.empty() ? move->dy : snap(move->dy);
    for (const auto& id : move->cards) {
      auto card =
          std::find_if(scene.cards.begin(), scene.cards.end(),
                       [&](const auto& value) { return value.id == id; });
      if (card == scene.cards.end())
        throw LayoutFailure("moved Card is missing: " + id);
      card->x = snap(card->x) + dx;
      card->y = snap(card->y) + dy;
      auto entry = json::Object(json::Get(positions, id, "{}"));
      entry["x"] = std::to_string(card->x);
      entry["y"] = std::to_string(card->y);
      positions[id] = json::Encode(entry);
    }
    auto notes = json::Array(json::Get(doc, "notes", "[]"));
    for (auto& raw : notes) {
      auto note = json::Object(raw);
      if (!has(move->notes, json::Text(note, "id"))) continue;
      note["x"] = std::to_string(std::round(number(note, "x", 0) + dx));
      note["y"] = std::to_string(std::round(number(note, "y", 0) + dy));
      raw = json::Encode(note);
    }
    if (!move->notes.empty()) doc["notes"] = json::Encode(notes);
  }
  const auto proposal =
      action < 0 ? layout::place_missing(scene, missing)
      : action == 0
          ? layout::arrange(scene)
          : layout::untangle(scene,
                             move ? move->cards : std::vector<std::string>{});
  for (const auto& p : proposal.cards) {
    auto position = json::Object(json::Get(positions, p.id, "{}"));
    position["x"] = std::to_string(p.x);
    position["y"] = std::to_string(p.y);
    positions[p.id] = json::Encode(position);
  }
  for (const auto& order : proposal.port_orders) {
    if (move && move->cards.empty()) break;
    auto position = json::Object(json::Get(positions, order.id, "{}"));
    std::vector<std::string> inputs, outputs;
    for (const auto& name : order.inputs) inputs.push_back(json::Quote(name));
    for (const auto& name : order.outputs) outputs.push_back(json::Quote(name));
    position["inputOrder"] = json::Encode(inputs);
    position["outputOrder"] = json::Encode(outputs);
    positions[order.id] = json::Encode(position);
  }
  if (action >= 0) {
    auto notes = json::Array(json::Get(doc, "notes", "[]"));
    for (auto& raw : notes) {
      auto note = json::Object(raw);
      for (const auto& p : proposal.notes)
        if (p.id == json::Text(note, "id")) {
          note["x"] = std::to_string(p.x);
          note["y"] = std::to_string(p.y);
        }
      raw = json::Encode(note);
    }
    if (!proposal.notes.empty()) doc["notes"] = json::Encode(notes);
  }
  doc["cards"] = json::Encode(positions);
  if (action == 0) doc.erase("boundary");
  return json::Encode(doc);
}

auto CompleteLayout(const std::string& project, const std::string& text,
                    const LayoutRead& read) -> std::string {
  return TransformLayout(project, text, read, -1);
}
auto ArrangeProjectLayout(const std::string& project, const std::string& text,
                          const LayoutRead& read, bool untangle,
                          const Tsugumi::Host::LayoutSceneT* measurements)
    -> std::string {
  return TransformLayout(project, text, read, untangle ? 1 : 0, measurements);
}

auto MoveProjectLayout(const std::string& project, const std::string& text,
                       const LayoutRead& read,
                       const Tsugumi::Host::ProjectMoveT& move) -> std::string {
  return TransformLayout(project, text, read, 1, move.scene.get(), &move);
}

auto PrepareLayoutSave(const std::string& arguments, const LayoutRead& read,
                       const SourcePatches& patches) -> std::string {
  auto args = json::Object(arguments);
  auto changes = json::Array(json::Get(args, "changes", "[]"));
  // A layout in a change: layout.bin bytes, or its JSON view (a model writes
  // the view; it is saved as layout.bin and the notes' Markdown files).
  struct Edit {
    size_t index;
    std::string view;
    bool source;
    bool written = false;  // given as a JSON view, with the notes' text
    bool validated = false;
  };
  struct Project {
    size_t index;
    std::string text;
    bool source;
  };
  std::map<std::string, Project> projects;
  std::map<std::string, Edit> layouts;
  std::set<std::string> deleted_projects;
  const auto current_view = [&](const std::string& prefix) {
    const auto file = read(prefix + "layout.bin");
    return file ? file->text : std::string("{}");
  };
  const auto view_of = [&](const std::string& prefix, const std::string& bytes,
                           bool& written) {
    written = bytes.starts_with('{');
    if (written) return bytes;
    return LayoutView(bytes, SplitLayoutView(current_view(prefix)).notes);
  };
  for (size_t i = 0; i < changes.size(); ++i) {
    const auto change = json::Object(changes[i]);
    const auto type = json::Text(change, "value_type");
    const auto value = json::Object(json::Get(change, "value", "{}"));
    if (type == "SetProject")
      projects[""] = {i, json::Text(value, "json"), false};
    if (type == "SetLayout") {
      bool written = false;
      auto view = view_of(
          "", DecodeSourceBytes(json::Get(value, "layout", "\"\"")), written);
      layouts[""] = {i, std::move(view), false, written};
    }
    const auto path = json::Text(value, "path");
    const bool layout = IsExternalDocument(path, "layout.bin");
    const bool project = IsExternalDocument(path, "tsugumi.json");
    if (!layout && !project) continue;
    const auto prefix = path.substr(0, path.size() - (layout ? 10 : 12));
    if (type == "RemoveSource") {
      if (project) deleted_projects.insert(prefix);
      if (layout) layouts[prefix] = {i, "{}", true};
      continue;
    }
    if (type == "PatchSource" && layout)
      throw LayoutFailure("layout.bin is binary; save its JSON view instead");
    if (type != "PutSource" && type != "PatchSource") continue;
    std::string text;
    if (type == "PutSource")
      text = Bytes(json::Get(value, "bytes"));
    else {
      const auto file = read(path);
      if (!file) throw LayoutFailure("missing project source for patch");
      text = Patched(*file, value, patches);
    }
    if (layout) {
      bool written = false;
      auto view = view_of(prefix, text, written);
      layouts[prefix] = {i, std::move(view), true, written};
    } else {
      projects[prefix] = {i, std::move(text), true};
    }
  }
  for (const auto& prefix : deleted_projects) layouts.erase(prefix);
  for (const auto& [prefix, project] : projects) {
    if (deleted_projects.contains(prefix)) continue;
    const auto layout_path = prefix + "layout.bin";
    const auto previous = read(prefix + "tsugumi.json");
    const auto before = previous
                            ? CardIds(previous->text, prefix + "tsugumi.json")
                            : std::set<std::string>{};
    const auto after = CardIds(project.text, prefix + "tsugumi.json");
    std::set<std::string> removed;
    for (const auto& id : before)
      if (!after.contains(id)) removed.insert(id);
    if (removed.empty()) continue;
    const auto file = read(layout_path);
    const auto old = layouts.contains(prefix) ? layouts.at(prefix).view
                     : file                   ? file->text
                                              : "{}";
    const auto cleaned = ValidateLayout(old, removed);
    if (layouts.contains(prefix)) {
      layouts.at(prefix).view = cleaned;
      layouts.at(prefix).validated = true;
    } else if (cleaned != old) {
      const auto i = changes.size();
      const auto value =
          project.source
              ? json::Members{{"path", json::Quote(layout_path)},
                              {"base_hash", file && file->hash
                                                ? json::Quote(*file->hash)
                                                : "null"}}
              : json::Members{};
      changes.push_back(json::Encode(json::Members{
          {"value_type",
           json::Quote(project.source ? "PutSource" : "SetLayout")},
          {"value", json::Encode(value)}}));
      layouts[prefix] = {i, cleaned, project.source, false, true};
    }
  }
  for (const auto& [prefix, layout] : layouts) {
    auto change = json::Object(changes.at(layout.index));
    auto value = json::Object(json::Get(change, "value"));
    if (json::Text(change, "value_type") == "RemoveSource" &&
        layout.view == "{}")
      continue;
    const auto view =
        layout.validated ? layout.view : ValidateLayout(layout.view);
    const auto split = SplitLayoutView(view);
    const auto bytes = json::Encode(json::Members{
        {"base64", json::Quote(crypto::base64(split.layout))}});
    if (layout.source) {
      value["bytes"] = bytes;
      change["value_type"] = json::Quote("PutSource");
      value.erase("patches");
    } else {
      value["layout"] = bytes;
    }
    change["value"] = json::Encode(value);
    changes[layout.index] = json::Encode(change);
    if (!layout.written) continue;
    // A written view also says what its notes read.
    const auto before = SplitLayoutView(current_view(prefix)).notes;
    for (const auto& [id, text] : split.notes) {
      const auto was = before.find(id);
      if (was != before.end() && was->second == text) continue;
      json::Members put{{"path", json::Quote(NotePath(prefix, id))},
                        {"bytes", json::Quote(text)}};
      if (was != before.end())
        put["base_hash"] = json::Quote(crypto::sha256_hex(was->second));
      changes.push_back(json::Encode(json::Members{
          {"value_type", json::Quote("PutSource")},
          {"value", json::Encode(put)}}));
    }
    for (const auto& [id, text] : before)
      if (!split.notes.contains(id))
        changes.push_back(json::Encode(json::Members{
            {"value_type", json::Quote("RemoveSource")},
            {"value", json::Encode(json::Members{
                          {"path", json::Quote(NotePath(prefix, id))},
                          {"base_hash",
                           json::Quote(crypto::sha256_hex(text))}})}}));
  }
  args["changes"] = json::Encode(changes);
  return json::Encode(args);
}
}  // namespace tsugumi::host
