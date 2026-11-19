#include "host/project_operations.hh"

#include <algorithm>
#include <map>
#include <set>

#include "agent/provider.hh"
#include "agent/save_policy.hh"
#include "host/document_edits.hh"
#include "host/failure.hh"
#include "host/layout_service.hh"
#include "host/projects.hh"
#include "host/templates.hh"
#include "tsugumi/document/document.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi::host {
namespace {
namespace H = Tsugumi::Host;
namespace json = ::tsugumi::json;
auto Value(const OperationReply& reply) -> json::Members {
  const auto envelope = json::Object(reply.json);
  const auto value = json::Object(json::Get(envelope, "value", "{}"));
  if (json::Text(envelope, "reply") == "Failure")
    throw Failure(json::Text(value, "kind"), json::Text(value, "diagnostic"));
  return value;
}
auto Bytes(const std::string& value) -> std::string {
  if (value.starts_with('"')) return json::String(value);
  std::string decoded;
  if (!crypto::base64_decode(json::Text(json::Object(value), "base64"),
                             decoded))
    throw Failure("invalid_request", "invalid file bytes");
  return decoded;
}
auto Change(const std::string& kind, json::Members value) -> std::string {
  return json::Encode(json::Members{{"value_type", json::Quote(kind)},
                                    {"value", json::Encode(value)}});
}
auto Prefix(const std::string& path) -> std::string {
  if (path.empty()) return {};
  if (!fs::valid_relative_path(path) ||
      !IsExternalDocument(path + "/tsugumi.json", "tsugumi.json"))
    throw Failure("invalid_path", "project_path must name an external project");
  return path + '/';
}
auto Intent(const H::ProjectOperateRequestT& operation,
            const std::string& origin) -> std::string {
  auto semantic = operation;
  semantic.op_id.clear();
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(H::ProjectOperateRequest::Pack(builder, &semantic));
  crypto::Sha256 hash;
  hash.update(origin + '\n');
  hash.update(builder.GetBufferPointer(), builder.GetSize());
  return hash.finish_hex();
}
// Expand transformations into guarded file edits against this inspection only.
auto Prepare(
    const H::ProjectOperationBodyUnion& body, const SaveBase& base,
    const EditFileRead& read,
    const std::function<void(const std::string&)>& authorize,
    const std::function<std::filesystem::path(const std::string&)>& demo)
    -> json::Members {
  if (const auto* edits = body.AsProjectEdits())
    return PrepareProjectEdits(*edits, base, read, authorize);
  if (const auto* replacement = body.AsProjectReplaceDocuments()) {
    if (replacement->expected_rev !=
        json::UInt(json::Get(base.snapshot, "rev")))
      throw Failure("revision_conflict",
                    "Raw document revision changed; reload before applying");
    H::ProjectEditsT edits;
    for (const auto& document : replacement->documents) {
      if (authorize) authorize(document->path);
      const auto old = base.files.contains(document->path)
                           ? std::optional(base.files.at(document->path))
                           : read(document->path);
      auto entry = std::make_unique<H::ProjectEditT>();
      if (document->present) {
        H::ProjectFilePutT put;
        put.file_path = document->path;
        put.expect_missing = !old;
        if (old) put.expected_hash = crypto::sha256_hex(*old);
        put.bytes.assign(document->json.begin(), document->json.end());
        entry->value.Set(std::move(put));
      } else if (old) {
        H::ProjectFileRemoveT remove;
        remove.file_path = document->path;
        remove.expected_hash = crypto::sha256_hex(*old);
        entry->value.Set(std::move(remove));
      } else
        continue;
      edits.edits.push_back(std::move(entry));
    }
    if (edits.edits.empty())
      return {{"base_rev", json::Get(base.snapshot, "rev")}, {"changes", "[]"}};
    return PrepareProjectEdits(edits, base, read, authorize);
  }
  H::ProjectEditsT edits;
  std::map<std::string, std::optional<std::string>> staged;
  std::set<std::string> paths;
  for (const auto& [path, ignored] : base.files) {
    (void)ignored;
    paths.insert(path);
  }
  for (const auto& raw : json::Array(json::Get(base.snapshot, "sources", "[]")))
    paths.insert(json::Text(json::Object(raw), "path"));
  auto original = [&](const std::string& path) -> std::optional<std::string> {
    if (authorize) authorize(path);
    if (base.files.contains(path)) return base.files.at(path);
    return read(path);
  };
  auto get = [&](const std::string& path) -> std::optional<std::string> {
    if (staged.contains(path)) return staged.at(path);
    return original(path);
  };
  auto put = [&](const std::string& path, std::string bytes) {
    staged[path] = std::move(bytes);
  };
  auto remove = [&](const std::string& path) { staged[path] = std::nullopt; };
  auto empty = [&](const std::string& prefix) {
    for (const auto& path : paths)
      if (path.starts_with(prefix))
        throw Failure("value_conflict", "destination already exists: " + path);
    for (const auto& [path, value] : staged)
      if (value && path.starts_with(prefix))
        throw Failure("value_conflict", "destination already exists: " + path);
  };
  auto required = [&](const std::string& path) {
    const auto bytes = get(path);
    if (!bytes) throw Failure("not_found", path);
    return *bytes;
  };
  std::string prefix;
  if (const auto* selection = body.AsProjectSelection())
    prefix = Prefix(selection->project_path);
  else if (const auto* connect = body.AsProjectConnect())
    prefix = Prefix(connect->project_path);
  else if (const auto* subgraph = body.AsProjectMakeSubgraph())
    prefix = Prefix(subgraph->project_path);
  else if (const auto* layout = body.AsProjectLayout())
    prefix = Prefix(layout->project_path);
  else if (const auto* move = body.AsProjectMove())
    prefix = Prefix(move->project_path);
  else if (const auto* copy = body.AsProjectCopy())
    prefix = Prefix(copy->project_path);
  else if (const auto* imported = body.AsProjectImport())
    prefix = Prefix(imported->project_path);
  else if (const auto* create = body.AsProjectCreateCard())
    prefix = Prefix(create->project_path);
  else
    throw Failure("invalid_request", "missing operation body");
  const auto project = required(prefix + "tsugumi.json");
  const auto layout_path = prefix + "layout.json";
  auto read_layout = [&] { return get(layout_path).value_or("{}"); };
  auto read_geometry =
      [&](const std::string& relative) -> std::optional<LayoutFile> {
    const auto bytes = get(prefix + relative);
    if (!bytes) return std::nullopt;
    return LayoutFile{*bytes, {}};
  };
  auto displayed_positions = [&](const std::string& layout) {
    return json::Object(
        json::Get(json::Object(CompleteLayout(project, layout, read_geometry)),
                  "cards", "{}"));
  };
  auto external_id = [&](const std::string& card) {
    return json::Text(
        json::Object(
            get(prefix + "definitions/" + card + "/_card.json").value_or("{}")),
        "external");
  };
  auto retained_externals = [&](const std::set<std::string>& removed) {
    std::set<std::string> retained;
    const auto definitions = prefix + "definitions/";
    for (const auto& [path, bytes] : base.files) {
      if (!path.starts_with(definitions) || !path.ends_with("/_card.json"))
        continue;
      const auto local = std::string_view(path).substr(definitions.size());
      const auto slash = local.find('/');
      if (slash == local.rfind('/') &&
          !removed.contains(std::string(local.substr(0, slash))))
        retained.insert(json::Text(json::Object(bytes, path), "external"));
    }
    return retained;
  };
  auto place = [&](const std::string& id, double x, double y,
                   bool automatic = false) {
    if (!valid_id(id))
      throw Failure("invalid_request", "invalid Card id: " + id);
    auto root = json::Object(required(prefix + "tsugumi.json"),
                             prefix + "tsugumi.json");
    auto graph = json::Object(json::Get(root, "canvas", "{}"),
                              prefix + "tsugumi.json.canvas");
    auto cards = json::Array(json::Get(graph, "cards", "[]"),
                             prefix + "tsugumi.json.canvas.cards");
    for (const auto& raw : cards)
      if (json::Text(json::Object(raw), "id") == id)
        throw Failure("value_conflict", "Card already exists: " + id);
    cards.push_back(json::Encode(json::Members{{"id", json::Quote(id)}}));
    graph["cards"] = json::Encode(cards);
    root["canvas"] = json::Encode(graph);
    put(prefix + "tsugumi.json", json::Encode(root));
    for (const auto& field : {"displays", "boundaries"}) {
      auto layout = json::Object(get(prefix + "layout.json").value_or("{}"),
                                 prefix + "layout.json");
      if (!layout.contains(field)) continue;
      auto entries =
          json::Object(layout.at(field), prefix + "layout.json." + field);
      std::erase_if(entries, [&](const auto& entry) {
        return entry.first == id || entry.first.starts_with(id + '/');
      });
      layout[field] = json::Encode(entries);
      put(prefix + "layout.json", json::Encode(layout));
    }
    if (!automatic) {
      auto layout = json::Object(get(prefix + "layout.json").value_or("{}"),
                                 prefix + "layout.json");
      auto positions = json::Object(json::Get(layout, "cards", "{}"),
                                    prefix + "layout.json.cards");
      positions[id] = json::Encode(
          json::Members{{"x", std::to_string(x)}, {"y", std::to_string(y)}});
      layout["cards"] = json::Encode(positions);
      put(prefix + "layout.json", json::Encode(layout));
    }
  };
  auto definition = [&](const std::string& dir, const std::string& raw) {
    const auto parts = json::Object(raw);
    for (const auto& [key, name] :
         {std::pair{"card", "_card.json"}, {"meta", "_meta.json"}})
      if (parts.contains(key)) put(dir + name, parts.at(key));
  };
  auto external = [&](const std::string& to, const std::string& raw) {
    const auto state = json::Object(raw);
    put(to + "tsugumi.json", json::Get(state, "project", "{}"));
    put(to + "layout.json", json::Get(state, "layout", "{}"));
    for (const auto& [id, d] :
         json::Object(json::Get(state, "definitions", "{}")))
      definition(to + "definitions/" + id + '/', d);
  };
  auto link = [&](const std::string& id, const std::string& name) {
    definition(prefix + "definitions/" + id + '/',
               json::Encode(json::Members{
                   {"card",
                    json::Encode(json::Members{{"external", json::Quote(id)}})},
                   {"meta", json::Encode(
                                json::Members{{"name", json::Quote(name)}})}}));
  };
  auto document_state = [&](const auto& files) {
    auto scope = [&](const std::string& at) {
      json::Members state{{"project", files.at(at + "tsugumi.json")},
                          {"layout", files.contains(at + "layout.json")
                                         ? files.at(at + "layout.json")
                                         : "{}"}};
      json::Members definitions;
      for (const auto& [path, bytes] : files) {
        if (!path.starts_with(at + "definitions/")) continue;
        const auto rest = path.substr(at.size() + 12);
        const auto slash = rest.find('/');
        if (slash == std::string::npos ||
            rest.find('/', slash + 1) != std::string::npos)
          continue;
        const auto file = rest.substr(slash + 1), id = rest.substr(0, slash);
        std::string key = file == "_card.json"   ? "card"
                          : file == "_meta.json" ? "meta"
                                                 : "";
        if (key.empty()) continue;
        auto d = json::Object(json::Get(definitions, id, "{}"));
        d[key] = bytes;
        definitions[id] = json::Encode(d);
      }
      state["definitions"] = json::Encode(definitions);
      return json::Encode(state);
    };
    auto state = json::Object(scope(""));
    json::Members externals;
    for (const auto& [path, ignored] : files) {
      (void)ignored;
      if (IsExternalDocument(path, "tsugumi.json")) {
        const auto at = path.substr(0, path.size() - 12);
        externals[at] = scope(at);
      }
    }
    state["externals"] = json::Encode(externals);
    return json::Encode(state);
  };
  if (const auto* selection = body.AsProjectSelection()) {
    if (selection->action == H::ProjectSelectionAction::Place) {
      for (const auto& id : selection->cards) {
        if (!get(prefix + "definitions/" + id + "/_card.json"))
          throw Failure("target_missing", "Definition: " + id);
        place(id, selection->x, selection->y);
      }
    } else {
      const std::set<std::string> ids(selection->cards.begin(),
                                      selection->cards.end());
      for (const auto& id : ids)
        if (!valid_id(id))
          throw Failure("invalid_request", "invalid Card id: " + id);
      auto root = json::Object(project, prefix + "tsugumi.json");
      auto graph = json::Object(json::Get(root, "canvas", "{}"),
                                prefix + "tsugumi.json.canvas");
      auto cards = json::Array(json::Get(graph, "cards", "[]"),
                               prefix + "tsugumi.json.canvas.cards");
      const bool deleting =
          selection->action == H::ProjectSelectionAction::Delete;
      const bool definitions =
          selection->action == H::ProjectSelectionAction::DeleteDefinitions;
      if (definitions) {
        for (const auto& raw : cards)
          if (ids.contains(json::Text(json::Object(raw), "id")))
            throw Failure("value_conflict",
                          "Cannot delete a placed definition");
      }
      if (deleting || definitions) {
        const auto retained = retained_externals(ids);
        for (const auto& id : ids) {
          const auto dir = prefix + "definitions/" + id + '/';
          std::vector<std::string> roots{dir};
          const auto external = external_id(id);
          if (!external.empty() && !retained.contains(external)) {
            if (!valid_id(external)) throw Failure("invalid_path", external);
            roots.push_back(prefix + "externals/" + external + '/');
          }
          for (const auto& path : paths)
            for (const auto& at : roots)
              if (path.starts_with(at)) remove(path);
        }
      }
      if (!definitions) {
        if (deleting) {
          std::erase_if(cards, [&](const auto& raw) {
            return ids.contains(json::Text(json::Object(raw), "id"));
          });
          graph["cards"] = json::Encode(cards);
        }
        auto wires = json::Array(json::Get(graph, "connections", "[]"),
                                 prefix + "tsugumi.json.canvas.connections");
        const auto selected_wires = json::Array(
            selection->connections_json.empty() ? "[]"
                                                : selection->connections_json,
            "connections_json");
        std::erase_if(wires, [&](const auto& raw) {
          const auto wire = json::Object(raw);
          const auto from =
              json::Text(json::Object(json::Get(wire, "from", "{}")), "card");
          const auto to =
              json::Text(json::Object(json::Get(wire, "to", "{}")), "card");
          if (deleting) return ids.contains(from) || ids.contains(to);
          if (selection->action == H::ProjectSelectionAction::DisconnectInputs)
            return ids.contains(to);
          if (selection->action == H::ProjectSelectionAction::DisconnectOutputs)
            return ids.contains(from);
          return std::any_of(selected_wires.begin(), selected_wires.end(),
                             [&](const auto& selected) {
                               return DocumentsEqual(raw, selected);
                             });
        });
        graph["connections"] = json::Encode(wires);
        root["canvas"] = json::Encode(graph);
        if (deleting) {
          auto boundary = json::Object(json::Get(root, "boundary", "{}"),
                                       prefix + "tsugumi.json.boundary");
          for (const auto& side : {"inputs", "outputs"}) {
            if (!boundary.contains(side)) continue;
            auto ends = json::Object(boundary.at(side),
                                     prefix + "tsugumi.json.boundary." + side);
            std::erase_if(ends, [&](const auto& item) {
              return ids.contains(
                  json::Text(json::Object(item.second), "card"));
            });
            boundary[side] = json::Encode(ends);
          }
          if (root.contains("boundary"))
            root["boundary"] = json::Encode(boundary);
          auto layout = json::Object(get(prefix + "layout.json").value_or("{}"),
                                     prefix + "layout.json");
          for (const auto& field : {"cards", "displays", "boundaries"}) {
            if (!layout.contains(field)) continue;
            auto values =
                json::Object(layout.at(field), prefix + "layout.json." + field);
            std::erase_if(values, [&](const auto& item) {
              return std::any_of(ids.begin(), ids.end(), [&](const auto& id) {
                return item.first == id || item.first.starts_with(id + '/');
              });
            });
            layout[field] = json::Encode(values);
          }
          if (layout.contains("notes")) {
            auto notes =
                json::Array(layout.at("notes"), prefix + "layout.json.notes");
            size_t note_index = 0;
            std::erase_if(notes, [&](const auto& raw) {
              const auto id = json::Text(
                  json::Object(raw, prefix + "layout.json.notes[" +
                                        std::to_string(note_index++) + "]"),
                  "id");
              return std::find(selection->notes.begin(), selection->notes.end(),
                               id) != selection->notes.end();
            });
            layout["notes"] = json::Encode(notes);
          }
          auto displays = json::Object(json::Get(layout, "displays", "{}"),
                                       prefix + "layout.json.displays");
          for (const auto& id : selection->frames) {
            if (ids.contains(id)) continue;
            auto frame = json::Object(json::Get(displays, id, "{}"),
                                      prefix + "layout.json.displays." + id);
            if (selection->scene) {
              for (const auto& box : selection->scene->frames) {
                if (!box || box->id != id) continue;
                for (const auto& [field, value] : {std::pair{"x", box->x},
                                                   {"y", box->y},
                                                   {"width", box->width},
                                                   {"height", box->height}})
                  if (!frame.contains(field))
                    frame[field] = std::to_string(value);
              }
            }
            frame["hidden"] = "true";
            displays[id] = json::Encode(frame);
          }
          if (!selection->frames.empty())
            layout["displays"] = json::Encode(displays);
          if (layout.contains("groups")) {
            auto groups =
                json::Array(layout.at("groups"), prefix + "layout.json.groups");
            size_t group_index = 0;
            for (auto& raw : groups) {
              const auto at = prefix + "layout.json.groups[" +
                              std::to_string(group_index++) + "]";
              auto group = json::Object(raw, at);
              for (const auto& field : {"cards", "outputs"}) {
                if (!group.contains(field)) continue;
                auto members = json::Array(group.at(field), at + "." + field);
                size_t member_index = 0;
                std::erase_if(members, [&](const auto& member) {
                  return ids.contains(
                      std::string(field) == "cards"
                          ? json::String(member)
                          : json::Text(
                                json::Object(
                                    member, at + ".outputs[" +
                                                std::to_string(member_index++) +
                                                "]"),
                                "card"));
                });
                group[field] = json::Encode(members);
              }
              raw = json::Encode(group);
            }
            layout["groups"] = json::Encode(groups);
          }
          put(prefix + "layout.json", json::Encode(layout));
        }
        put(prefix + "tsugumi.json", json::Encode(root));
      }
    }
  } else if (const auto* connect = body.AsProjectConnect()) {
    json::Members definitions;
    for (const auto& raw : json::Array(
             json::Get(json::Object(json::Get(
                           json::Object(project, prefix + "tsugumi.json"),
                           "canvas", "{}")),
                       "cards", "[]"))) {
      const auto id = json::Text(json::Object(raw), "id");
      json::Members parts;
      for (const auto& [part, name] :
           {std::pair{"card", "_card.json"}, {"meta", "_meta.json"}}) {
        const auto path = prefix + "definitions/" + id + '/' + name;
        // Only definitions that influence this connection need a helper read
        // grant.
        if (base.files.contains(path)) parts[part] = base.files.at(path);
      }
      definitions[id] = json::Encode(parts);
    }
    const auto connected = ConnectDocuments(
        json::Encode(json::Members{{"project", project},
                                   {"definitions", json::Encode(definitions)}}),
        connect->from_card, connect->from_port, connect->to_card,
        connect->to_port, connect->fork_id);
    if (connected.needs_fork)
      throw Failure("invalid_request",
                    "Reserve a fork Card id and supply fork_id");
    const auto state = json::Object(connected.state_json);
    put(prefix + "tsugumi.json", state.at("project"));
    for (const auto& [id, raw] : json::Object(state.at("definitions"))) {
      const auto parts = json::Object(raw);
      for (const auto& [part, name] :
           {std::pair{"card", "_card.json"}, {"meta", "_meta.json"}})
        if (parts.contains(part) && (!definitions.contains(id) ||
                                     json::Get(json::Object(definitions.at(id)),
                                               part) != parts.at(part)))
          put(prefix + "definitions/" + id + '/' + name, parts.at(part));
    }
  } else if (const auto* create = body.AsProjectCreateCard()) {
    empty(prefix + "definitions/" + create->id + '/');
    const auto prepared = WriteTemplate(create->template_, create->id);
    auto state = json::Object(prepared.definition_json);
    auto meta = json::Object(json::Get(state, "meta", "{}"));
    if (!create->name.empty()) meta["name"] = json::Quote(create->name);
    state["meta"] = json::Encode(meta);
    const auto dir = prefix + "definitions/" + create->id + '/';
    definition(dir, json::Encode(state));
    for (const auto& file : prepared.files) put(dir + file.path, file.text);
    place(create->id, create->x, create->y, create->automatic_placement);
  } else if (const auto* imported = body.AsProjectImport()) {
    empty(prefix + "definitions/" + imported->id + '/');
    const auto destination = prefix + "externals/" + imported->id + '/';
    empty(destination);
    for (const auto& [path, bytes] :
         ReadProjectFiles(imported->path, authorize))
      put(destination + path, bytes);
    link(imported->id,
         std::filesystem::path(imported->path).filename().string());
    place(imported->id, imported->x, imported->y);
  } else if (const auto* copy = body.AsProjectCopy()) {
    if (copy->items.empty())
      throw Failure("invalid_request", "choose Cards to copy");
    const auto state = document_state(base.files);
    const auto positions = copy->copy_connections
                               ? displayed_positions(read_layout())
                               : json::Members{};
    std::map<std::string, std::string> copied_ids;
    for (const auto& item : copy->items) {
      if (!valid_id(item->id))
        throw Failure("invalid_request", "invalid copy item");
      const auto to = prefix + "definitions/" + item->id + '/';
      empty(to);
      empty(prefix + "externals/" + item->id + '/');
      const bool is_external =
          IsExternalDocument(item->from + "tsugumi.json", "tsugumi.json");
      const auto destination =
          is_external ? prefix + "externals/" + item->id + '/' : to;
      auto source_state = state;
      auto source_paths = std::vector<std::string>(paths.begin(), paths.end());
      std::map<std::string, std::string> imported_files;
      std::filesystem::path source_root;
      auto from = item->from;
      const bool imported = !item->demo.empty() || !item->folder.empty();
      if (imported) {
        if ((!item->demo.empty() && (!demo || !item->folder.empty())) ||
            copy->copy_connections || !valid_id(item->from))
          throw Failure("invalid_request",
                        "Choose a source project and a Card id");
        source_root = item->demo.empty() ? std::filesystem::path(item->folder)
                                         : demo(item->demo);
        imported_files = ReadProjectFiles(
            source_root.string(), item->folder.empty() ? nullptr : authorize);
        source_paths.clear();
        for (const auto& [path, bytes] : imported_files) {
          (void)bytes;
          source_paths.push_back(path);
        }
        source_state = document_state(imported_files);
        from = "definitions/" + item->from + '/';
      }
      const auto& source_files = imported ? imported_files : base.files;
      if (!source_files.contains(from + "_card.json") && !is_external)
        throw Failure("not_found", "Card not found");
      std::vector<std::string> closure{from};
      if (!is_external) {
        const auto external_id = json::Text(
            json::Object(source_files.at(from + "_card.json")), "external");
        if (!external_id.empty())
          closure.push_back(from.substr(0, from.rfind("definitions/")) +
                            "externals/" + external_id + '/');
      }
      for (const auto& path : source_paths)
        for (const auto& dir : closure)
          if (path.starts_with(dir) && authorize)
            authorize(imported ? (source_root / path).string() : path);
      const auto patch = CopyDocuments(source_state, source_paths, from,
                                       destination, item->name);
      const auto values = json::Object(patch.edits_json);
      for (const auto& [dir, raw] :
           json::Object(json::Get(values, "definitions", "{}")))
        definition(dir, raw);
      for (const auto& [dir, raw] :
           json::Object(json::Get(values, "externals", "{}"))) {
        empty(dir);
        external(dir, raw);
      }
      for (const auto& file : patch.files)
        put(file.to,
            imported ? imported_files.at(file.from) : required(file.from));
      if (is_external)
        link(item->id, item->name.empty() ? item->id : item->name);
      double x = item->x, y = item->y;
      if (copy->copy_connections) {
        if (!item->from.starts_with(prefix + "definitions/"))
          throw Failure("invalid_request", "duplicate Cards in the same scope");
        const auto old = item->from.substr(
            prefix.size() + 12, item->from.size() - prefix.size() - 13);
        copied_ids[old] = item->id;
        if (!positions.contains(old))
          throw Failure("not_found", "Card is missing from the canvas: " + old);
        const auto position = json::Object(positions.at(old));
        x = std::stod(position.at("x")) + 30;
        y = std::stod(position.at("y")) + 30;
      }
      place(item->id, x, y);
    }
    if (copy->copy_connections) {
      auto root = json::Object(required(prefix + "tsugumi.json"),
                               prefix + "tsugumi.json");
      auto graph = json::Object(json::Get(root, "canvas", "{}"),
                                prefix + "tsugumi.json.canvas");
      auto wires = json::Array(json::Get(graph, "connections", "[]"),
                               prefix + "tsugumi.json.canvas.connections");
      const auto previous = wires;
      for (const auto& raw : previous) {
        auto wire = json::Object(raw);
        auto from = json::Object(json::Get(wire, "from", "{}")),
             to = json::Object(json::Get(wire, "to", "{}"));
        const auto f = json::Text(from, "card"), t = json::Text(to, "card");
        if (!copied_ids.contains(f) || !copied_ids.contains(t)) continue;
        from["card"] = json::Quote(copied_ids.at(f));
        to["card"] = json::Quote(copied_ids.at(t));
        wire["from"] = json::Encode(from);
        wire["to"] = json::Encode(to);
        wires.push_back(json::Encode(wire));
      }
      graph["connections"] = json::Encode(wires);
      root["canvas"] = json::Encode(graph);
      put(prefix + "tsugumi.json", json::Encode(root));
    }
  } else if (const auto* subgraph = body.AsProjectMakeSubgraph()) {
    const auto nested = MakeSubgraph(project, subgraph->cards, subgraph->id);
    const auto destination = prefix + "externals/" + subgraph->id + '/';
    if (get(destination + "tsugumi.json") ||
        get(prefix + "definitions/" + subgraph->id + "/_card.json"))
      throw Failure("value_conflict", "subgraph destination already exists");
    put(prefix + "tsugumi.json", nested.project_json);
    put(destination + "tsugumi.json",
        PreserveProjectBytes(project, nested.external_json));
    put(prefix + "definitions/" + subgraph->id + "/_card.json",
        nested.card_json);
    auto outer_layout = json::Object(read_layout(), layout_path);
    const auto positions = displayed_positions(json::Encode(outer_layout));
    json::Members inner_positions;
    const auto retained =
        retained_externals({subgraph->cards.begin(), subgraph->cards.end()});
    std::set<std::string> copied;
    for (const auto& id : subgraph->cards) {
      const auto definition = prefix + "definitions/" + id + '/';
      const auto target = external_id(id);
      const auto external =
          target.empty() ? std::string{} : prefix + "externals/" + target + '/';
      for (const auto& path : paths) {
        if (!path.starts_with(definition) &&
            (external.empty() || !path.starts_with(external)))
          continue;
        if (!copied.insert(path).second) continue;
        put(destination + path.substr(prefix.size()), required(path));
        // Root definition removal includes its sources. External files and
        // nested definitions use the ordinary concrete source removals.
        if (path.starts_with(definition) || !retained.contains(target))
          remove(path);
      }
      inner_positions[id] = positions.at(id);
    }
    double cx = 0, cy = 0;
    for (const auto& id : subgraph->cards) {
      const auto pos = json::Object(positions.at(id));
      cx += std::stod(pos.at("x"));
      cy += std::stod(pos.at("y"));
    }
    cx /= subgraph->cards.size();
    cy /= subgraph->cards.size();
    for (auto& [id, raw] : inner_positions) {
      auto pos = json::Object(raw, prefix + "layout.json.cards." + id);
      pos["x"] = std::to_string(std::stod(pos.at("x")) - cx);
      pos["y"] = std::to_string(std::stod(pos.at("y")) - cy);
      raw = json::Encode(pos);
    }
    put(destination + "layout.json",
        json::Encode(json::Members{{"cards", json::Encode(inner_positions)}}));
    auto selected = [&](const std::string& id) {
      return std::find(subgraph->cards.begin(), subgraph->cards.end(), id) !=
             subgraph->cards.end();
    };
    for (const auto& field : {"cards", "displays", "boundaries"}) {
      auto entries = json::Object(json::Get(outer_layout, field, "{}"),
                                  prefix + "layout.json." + field);
      std::erase_if(entries, [&](const auto& entry) {
        const auto slash = entry.first.find('/');
        return selected(entry.first.substr(0, slash));
      });
      if (field == std::string("cards"))
        entries[subgraph->id] = json::Encode(json::Members{
            {"x", std::to_string(cx)}, {"y", std::to_string(cy)}});
      if (outer_layout.contains(field) || !entries.empty())
        outer_layout[field] = json::Encode(entries);
    }
    auto groups = json::Array(json::Get(outer_layout, "groups", "[]"),
                              prefix + "layout.json.groups");
    size_t group_index = 0;
    for (auto& raw : groups) {
      const auto at =
          prefix + "layout.json.groups[" + std::to_string(group_index++) + "]";
      auto group = json::Object(raw, at);
      auto members =
          json::Array(json::Get(group, "cards", "[]"), at + ".cards");
      std::erase_if(members,
                    [&](const auto& id) { return selected(json::String(id)); });
      group["cards"] = json::Encode(members);
      if (group.contains("outputs")) {
        auto outputs = json::Array(group.at("outputs"), at + ".outputs");
        size_t output_index = 0;
        std::erase_if(outputs, [&](const auto& output) {
          return selected(json::Text(
              json::Object(output, at + ".outputs[" +
                                       std::to_string(output_index++) + "]"),
              "card"));
        });
        group["outputs"] = json::Encode(outputs);
      }
      raw = json::Encode(group);
    }
    if (outer_layout.contains("groups"))
      outer_layout["groups"] = json::Encode(groups);
    put(prefix + "layout.json", json::Encode(outer_layout));
    if (!subgraph->name.empty())
      put(prefix + "definitions/" + subgraph->id + "/_meta.json",
          json::Encode(json::Members{{"name", json::Quote(subgraph->name)}}));
  } else if (const auto* move = body.AsProjectMove()) {
    put(layout_path,
        MoveProjectLayout(project, read_layout(), read_geometry, *move));
  } else if (const auto* layout = body.AsProjectLayout()) {
    if (layout->action != H::ProjectLayoutAction::Arrange &&
        layout->action != H::ProjectLayoutAction::Untangle)
      throw Failure("invalid_request", "unknown layout action");
    put(layout_path,
        ArrangeProjectLayout(project, read_layout(), read_geometry,
                             layout->action == H::ProjectLayoutAction::Untangle,
                             layout->scene.get()));
  }
  // Deleting a root definition is a guarded folder replacement; independent
  // _card.json removals remain invalid in the public edit vocabulary.
  std::set<std::string> removed;
  for (const auto& [path, value] : staged) {
    if (!value && path.ends_with("/_card.json")) {
      const auto dir = path.substr(0, path.size() - 10);
      if (removed.contains(dir)) continue;
      H::ProjectTreeEditT tree;
      tree.prefix = dir;
      for (const auto& file : paths) {
        if (!file.starts_with(dir)) continue;
        const auto bytes = original(file);
        if (!bytes) throw Failure("not_found", file);
        auto hash = std::make_unique<H::ProjectFileHashT>();
        hash->path = file;
        hash->hash = crypto::sha256_hex(*bytes);
        tree.expected.push_back(std::move(hash));
        const auto now = get(file);
        if (now) {
          auto entry = std::make_unique<H::ProjectFileT>();
          entry->path = file;
          entry->bytes.assign(now->begin(), now->end());
          tree.files.push_back(std::move(entry));
        }
      }
      auto entry = std::make_unique<H::ProjectEditT>();
      entry->value.Set(std::move(tree));
      edits.edits.push_back(std::move(entry));
      removed.insert(dir);
    }
  }
  for (const auto& [path, value] : staged) {
    if (std::any_of(removed.begin(), removed.end(),
                    [&](const auto& dir) { return path.starts_with(dir); }))
      continue;
    const auto before = original(path);
    if (before == value) continue;
    auto entry = std::make_unique<H::ProjectEditT>();
    if (value) {
      auto bytes = *value;
      if (before && base.files.contains(path))
        bytes = PreserveProjectBytes(*before, bytes);
      H::ProjectFilePutT edit;
      edit.file_path = path;
      edit.expect_missing = !before;
      if (before) edit.expected_hash = crypto::sha256_hex(*before);
      edit.bytes.assign(bytes.begin(), bytes.end());
      entry->value.Set(std::move(edit));
    } else {
      if (!before) continue;
      H::ProjectFileRemoveT edit;
      edit.file_path = path;
      edit.expected_hash = crypto::sha256_hex(*before);
      entry->value.Set(std::move(edit));
    }
    edits.edits.push_back(std::move(entry));
  }
  auto plan = edits.edits.empty()
                  ? json::Members{{"base_rev", json::Get(base.snapshot, "rev")},
                                  {"changes", "[]"}}
                  : PrepareProjectEdits(edits, base, read, authorize);
  auto changes = json::Array(plan.at("changes"));
  // An unchanged layout/connect still uses the existing save idempotence path.
  if (changes.empty())
    changes.push_back(Change("SetProject", {{"json", json::Quote(project)}}));
  plan["changes"] = json::Encode(changes);
  return plan;
}
// Preserve policy/placement changes relative to the bytes explicitly authored
// by this operation, so even an exact whitespace replacement is retained.
auto Authored(const json::Members& plan, SaveBase base) -> SaveBase {
  for (const auto& raw : json::Array(json::Get(plan, "changes", "[]"))) {
    const auto change = json::Object(raw);
    const auto type = json::Text(change, "value_type");
    const auto value = json::Object(json::Get(change, "value", "{}"));
    if (type == "SetProject" || type == "SetLayout")
      base.files[type == "SetProject" ? "tsugumi.json" : "layout.json"] =
          json::Text(value, "json");
    else if (type == "PutDefinition") {
      const auto dir = "definitions/" + json::Text(value, "id") + '/';
      for (const auto& [key, file] :
           {std::pair{"card_json", "_card.json"}, {"meta_json", "_meta.json"}})
        if (value.contains(key)) {
          const auto text = json::Text(value, key);
          if (text.empty())
            base.files.erase(dir + file);
          else
            base.files[dir + file] = text;
        }
    } else if (type == "PutSource" &&
               base.files.contains(json::Text(value, "path")))
      base.files[json::Text(value, "path")] = Bytes(json::Get(value, "bytes"));
  }
  return base;
}
// Policy and placement may return re-encoded JSON. Only their changed spans
// belong to this operation; every other document byte remains the original.
auto Preserve(json::Members plan, const SaveBase& base) -> json::Members {
  auto changes = json::Array(json::Get(plan, "changes", "[]"));
  for (auto& raw : changes) {
    auto change = json::Object(raw);
    auto value = json::Object(json::Get(change, "value", "{}"));
    const auto type = json::Text(change, "value_type");
    auto preserve = [&](const std::string& path, const std::string& key) {
      if (base.files.contains(path) && value.contains(key) &&
          json::Get(value, key) != "null")
        value[key] = json::Quote(
            PreserveProjectBytes(base.files.at(path), json::Text(value, key)));
    };
    if (type == "SetProject")
      preserve("tsugumi.json", "json");
    else if (type == "SetLayout")
      preserve("layout.json", "json");
    else if (type == "PutDefinition") {
      const auto dir = "definitions/" + json::Text(value, "id") + '/';
      for (const auto& [key, name] :
           {std::pair{"card_json", "_card.json"}, {"meta_json", "_meta.json"}})
        preserve(dir + name, key);
    } else if (type == "PutSource" &&
               base.files.contains(json::Text(value, "path"))) {
      const auto path = json::Text(value, "path");
      value["bytes"] = json::Quote(PreserveProjectBytes(
          base.files.at(path), Bytes(json::Get(value, "bytes"))));
    }
    change["value"] = json::Encode(value);
    raw = json::Encode(change);
  }
  plan["changes"] = json::Encode(changes);
  return plan;
}
}  // namespace

auto ProjectOperations::Execute(const H::ProjectOperateRequestT& operation,
                                const OperationContext& context,
                                const OperationCall& call) -> OperationReply {
  if (const auto* reload = operation.body.AsProjectReload()) {
    if (context.origin != "ui")
      throw Failure("invalid_request", "External reload is a human action");
    std::vector<std::string> approved;
    for (const auto& permission : reload->approved_permissions)
      approved.push_back(json::Quote(permission));
    return call("project_reload",
                {{"base_rev", std::to_string(reload->expected_rev)},
                 {"op_id", json::Quote(operation.op_id)},
                 {"origin", json::Quote(context.origin)},
                 {"check_permissions", "true"},
                 {"approved_permissions", json::Encode(approved)}});
  }
  const auto intent = Intent(operation, context.origin);
  // Reuse the core's existing save receipt, before interpreting old values that
  // a successful first application has already changed.
  const auto status =
      Value(call("operation_get", {{"op_id", json::Quote(operation.op_id)},
                                   {"save_intent", json::Quote(intent)}}));
  if (json::Text(status, "state") == "completed")
    return call("project_save", {{"op_id", json::Quote(operation.op_id)},
                                 {"save_intent", json::Quote(intent)},
                                 {"base_rev", "0"},
                                 {"origin", json::Quote(context.origin)},
                                 {"changes", "[]"}});
  for (int attempt = 0; attempt < 8; ++attempt) {
    try {
      const auto snapshot = Value(call("project_inspect", {}));
      const auto base = CaptureSaveBase(snapshot);
      auto read = [&](const std::string& path) -> std::optional<std::string> {
        std::string hash;
        for (const auto& raw :
             json::Array(json::Get(snapshot, "sources", "[]"))) {
          const auto source = json::Object(raw);
          if (json::Text(source, "path") == path)
            hash = json::Text(source, "hash");
        }
        if (hash.empty()) return std::nullopt;
        std::string bytes;
        uint64_t offset = 0;
        for (;;) {
          const auto page = Value(
              call("project_file_read", {{"path", json::Quote(path)},
                                         {"at_rev", json::Get(snapshot, "rev")},
                                         {"if_hash", json::Quote(hash)},
                                         {"offset", std::to_string(offset)},
                                         {"limit_bytes", "1048576"}}));
          const auto chunk = Bytes(json::Get(page, "bytes"));
          bytes += chunk;
          if (json::Bool(json::Get(page, "eof"))) break;
          const auto next = json::Get(page, "next_offset");
          offset = json::UInt(json::String(next, next));
        }
        return bytes;
      };
      auto plan = Prepare(
          operation.body, base, read,
          [&](const auto& path) {
            if (context.authorize_file) context.authorize_file(path, base);
          },
          context.demo);
      if (json::Array(plan.at("changes")).empty())
        plan["changes"] = json::Encode(std::vector<std::string>{
            Change("SetProject",
                   {{"json", json::Quote(base.files.at("tsugumi.json"))}})});
      const auto authored = Authored(plan, base);
      plan["op_id"] = json::Quote(operation.op_id);
      plan["origin"] = json::Quote(context.origin);
      if (context.origin == "agent" && operation.body.AsProjectEdits())
        plan =
            agent::PrepareAgentSave(plan, base,
                                    [&](const auto& method, const auto& args) {
                                      return Value(call(method, args));
                                    })
                .arguments;
      auto layout_read =
          [&](const std::string& path) -> std::optional<LayoutFile> {
        const auto bytes = base.files.find(path);
        if (bytes == base.files.end()) return std::nullopt;
        return LayoutFile{bytes->second, crypto::sha256_hex(bytes->second)};
      };
      plan = Preserve(
          json::Object(PrepareLayoutSave(json::Encode(plan), layout_read)),
          authored);
      if (context.authorize) context.authorize(plan, base);
      plan["check_permissions"] = context.origin == "agent" ? "true" : "false";
      plan["save_intent"] = json::Quote(intent);
      if (context.origin == "ui") {
        // Opaque to the core: carried in its existing save reply and replay.
        const auto inverse = ReverseProjectChanges(plan, base, read);
        flatbuffers::FlatBufferBuilder undo_builder;
        undo_builder.Finish(H::ProjectEdits::Pack(undo_builder, &inverse));
        plan["operation_data"] = json::Encode(json::Members{
            {"base64",
             json::Quote(crypto::base64(std::string(
                 reinterpret_cast<const char*>(undo_builder.GetBufferPointer()),
                 undo_builder.GetSize())))}});
      }
      const auto result = call("project_save", plan);
      const auto envelope = json::Object(result.json);
      if (operation.body.AsProjectReplaceDocuments()) return result;
      if (json::Text(envelope, "reply") != "Failure" ||
          json::Text(json::Object(json::Get(envelope, "value", "{}")),
                     "kind") != "revision_conflict")
        return result;
    } catch (const Failure& failure) {
      if (context.origin == "ui" && (failure.code == "value_conflict" ||
                                     failure.code == "source_changed")) {
        // Host guards fail before a core save. UIs receive the same typed
        // operation outcome as a conflict returned by core.
        Tsugumi::Data::FailureT value;
        value.kind = failure.code == "value_conflict"
                         ? Tsugumi::Data::FailureKind::value_conflict
                         : Tsugumi::Data::FailureKind::source_changed;
        value.diagnostic = failure.what();
        Tsugumi::RPC::ReplyT reply;
        reply.value.Set(std::move(value));
        Tsugumi::RPC::MessageT message;
        message.payload.Set(std::move(reply));
        flatbuffers::FlatBufferBuilder builder;
        Tsugumi::RPC::FinishMessageBuffer(
            builder, Tsugumi::RPC::Message::Pack(builder, &message));
        return {
            json::Encode(json::Members{
                {"reply", json::Quote("Failure")},
                {"value", json::Encode(json::Members{
                              {"kind", json::Quote(failure.code)},
                              {"diagnostic", json::Quote(failure.what())}})}}),
            std::string(
                reinterpret_cast<const char*>(builder.GetBufferPointer()),
                builder.GetSize())};
      }
      if (operation.body.AsProjectReplaceDocuments() ||
          failure.code != "revision_conflict")
        throw;
    } catch (const agent::Cancelled&) {
      throw;
    } catch (const ProjectFailure& failure) {
      throw Failure(failure.code, failure.what());
    } catch (const LayoutFailure& failure) {
      throw Failure("invalid_document", failure.what());
    } catch (const std::exception& failure) {
      throw Failure("validation_failed", failure.what());
    }
  }
  throw Failure("busy", "Project kept changing; retry the same operation id");
}
}  // namespace tsugumi::host
