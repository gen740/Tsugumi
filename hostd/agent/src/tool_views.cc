#include "agent/tool_views.hh"

#include <flatbuffers/idl.h>
#include <flatbuffers/reflection.h>

#include <algorithm>
#include <map>
#include <optional>
#include <set>

#include "host/save.hh"

namespace tsugumi::agent {
namespace {

auto ObjectOr(std::string_view raw) -> std::optional<json::Members> {
  if (raw.empty() || raw.front() != '{') return std::nullopt;
  try {
    return json::Object(raw);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

auto ArrayOr(std::string_view raw) -> std::vector<std::string> {
  if (raw.empty() || raw.front() != '[') return {};
  try {
    return json::Array(raw);
  } catch (const std::exception&) {
    return {};
  }
}

auto IsJson(const std::string& raw) -> bool {
  try {
    return json::Array("[" + raw + "]").size() == 1;
  } catch (const std::exception&) {
    return false;
  }
}

auto CardName(const std::string& card) -> std::string {
  std::string out;
  const auto object = ObjectOr(card);
  if (!object) return out;
  for (const auto& segment : ArrayOr(json::Get(*object, "segments", "[]")))
    out += (out.empty() ? "" : "/") + json::String(segment);
  return out;
}

auto DecodeValue(const std::string& schema, const std::string& root,
                 const std::string& bytes, std::string& error)
    -> std::optional<std::string> {
  const auto* schema_data = reinterpret_cast<const uint8_t*>(schema.data());
  flatbuffers::Verifier verifier(schema_data, schema.size());
  if (!reflection::VerifySchemaBuffer(verifier)) {
    error = "invalid schema";
    return std::nullopt;
  }
  const auto* reflected = reflection::GetSchema(schema_data);
  const auto* object = reflected->objects()->LookupByKey(root.c_str());
  if (!object) {
    error = "the schema has no " + root;
    return std::nullopt;
  }
  const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
  if (!flatbuffers::Verify(*reflected, *object, data, bytes.size())) {
    error = "the bytes are not a valid " + root;
    return std::nullopt;
  }
  flatbuffers::Parser parser;
  parser.opts.strict_json = true;
  parser.opts.output_default_scalars_in_json = true;
  parser.opts.indent_step = -1;
  if (!parser.Deserialize(schema_data, schema.size()) ||
      !parser.SetRootType(root.c_str())) {
    error = parser.error_.empty() ? "cannot load the schema" : parser.error_;
    return std::nullopt;
  }
  std::string text;
  if (const auto* failed = flatbuffers::GenerateText(parser, data, &text)) {
    error = failed;
    return std::nullopt;
  }
  return text;
}

// A value (inline_bytes) or a port (schema only): the binary schema is of no
// use to a model, the decoded value is.
auto ReadableValue(json::Members value) -> std::string {
  const auto inline_bytes = json::Get(value, "inline_bytes");
  if (inline_bytes != "null") {
    std::string error;
    std::optional<std::string> decoded;
    try {
      decoded = DecodeValue(host::DecodeSourceBytes(value.at("schema_bfbs")),
                            json::Text(value, "root_type"),
                            host::DecodeSourceBytes(inline_bytes), error);
    } catch (const std::exception& e) {
      error = e.what();
    }
    if (decoded && IsJson(*decoded)) {
      value["value"] = *decoded;
      value.erase("inline_bytes");
    } else {
      value["decode_error"] = json::Quote(error.empty() ? "not JSON" : error);
    }
  } else if (json::Get(value, "content_hash") != "null") {
    value["value_note"] =
        json::Quote("Not inline; read it with run_value_read.");
  }
  value.erase("schema_bfbs");
  return json::Encode(value);
}

auto Walk(const std::string& raw) -> std::string {
  if (raw.empty()) return raw;
  if (raw.front() == '[') {
    auto items = ArrayOr(raw);
    for (auto& item : items) item = Walk(item);
    return json::Encode(items);
  }
  auto object = ObjectOr(raw);
  if (!object) return raw;
  if (object->contains("schema_bfbs") && object->contains("root_type"))
    return ReadableValue(std::move(*object));
  for (auto& [key, value] : *object) value = Walk(value);
  return json::Encode(*object);
}

auto PortList(const std::string& raw) -> std::string {
  std::vector<std::string> out;
  for (const auto& item : ArrayOr(raw)) {
    const auto port = ObjectOr(item);
    if (!port) continue;
    auto text = json::Text(*port, "name");
    if (const auto type = json::Text(*port, "root_type"); !type.empty())
      text += ": " + type;
    out.push_back(json::Quote(text));
  }
  return json::Encode(out);
}

auto FrontMatter(const std::string& readme, std::string_view field)
    -> std::string {
  if (!readme.starts_with("---\n")) return {};
  const auto end = readme.find("\n---", 3);
  if (end == std::string::npos) return {};
  const auto head = readme.substr(3, end - 3);  // starts with a newline
  const auto key = "\n" + std::string(field) + ":";
  const auto at = head.find(key);
  if (at == std::string::npos) return {};
  auto line = head.substr(at + key.size());
  line = line.substr(0, line.find('\n'));
  const auto first = line.find_first_not_of(' ');
  return first == std::string::npos ? std::string{} : line.substr(first);
}

// One resolved Card: its ports, params and diagnostics.
auto CardSummary(const json::Members& resolved,
                 const json::Members* definition) -> std::string {
  json::Members card{{"card", json::Quote(CardName(json::Get(resolved, "card")))}};
  if (definition) {
    if (const auto settings = ObjectOr(json::Text(*definition, "card_json")))
      card["type"] = json::Get(*settings, "type", "null");
    const auto readme = json::Text(*definition, "readme");
    for (const auto* field : {"title", "description"})
      if (auto text = FrontMatter(readme, field); !text.empty())
        card[field] = json::Quote(text);
  }
  card["inputs"] = PortList(json::Get(resolved, "inputs", "[]"));
  card["outputs"] = PortList(json::Get(resolved, "outputs", "[]"));
  json::Members params;
  for (const auto& item : ArrayOr(json::Get(resolved, "parameters", "[]"))) {
    const auto param = ObjectOr(item);
    if (!param) continue;
    const auto value = json::Text(*param, "value_json");
    params[json::Text(*param, "name")] = IsJson(value) ? value : json::Quote(value);
  }
  if (!params.empty()) card["params"] = json::Encode(params);
  if (const auto diagnostics = json::Get(resolved, "diagnostics", "[]");
      !ArrayOr(diagnostics).empty())
    card["diagnostics"] = diagnostics;
  return json::Encode(card);
}

auto WireEnd(const std::string& raw) -> std::string {
  const auto end = ObjectOr(raw);
  if (!end) return "?";
  const auto port = json::Text(*end, "port");
  return json::Text(*end, "card") + (port.empty() ? "" : "." + port);
}

auto RunSummary(const std::string& raw) -> std::string {
  const auto run = ObjectOr(raw);
  if (!run) return raw;
  json::Members out;
  for (const auto* field : {"run_id", "state", "failure", "commit"})
    if (const auto value = json::Get(*run, field); value != "null" && value != "\"\"")
      out[field] = value;
  if (json::Bool(json::Get(*run, "partial_run"))) out["partial_run"] = "true";
  return json::Encode(out);
}

}  // namespace

auto DecodeRecordedValues(std::string reply) -> std::string {
  if (reply.find("\"schema_bfbs\"") == std::string::npos) return reply;
  return Walk(reply);
}

auto InspectSummary(const json::Members& snapshot) -> json::Members {
  json::Members out;
  for (const auto* field : {"project_id", "rev", "execution_rev"})
    out[field] = json::Get(snapshot, field);
  std::vector<std::string> wires;
  if (auto project = ObjectOr(json::Text(snapshot, "project_json"))) {
    if (const auto canvas = ObjectOr(json::Get(*project, "canvas", "{}")))
      for (const auto& raw : ArrayOr(json::Get(*canvas, "connections", "[]")))
        if (const auto wire = ObjectOr(raw))
          wires.push_back(json::Quote(WireEnd(json::Get(*wire, "from")) + " -> " +
                                      WireEnd(json::Get(*wire, "to"))));
    project->erase("canvas");
    out["settings"] = json::Encode(*project);
  }
  std::map<std::string, json::Members> definitions;
  for (const auto& raw : ArrayOr(json::Get(snapshot, "definitions", "[]")))
    if (auto definition = ObjectOr(raw))
      definitions[json::Text(*definition, "id")] = std::move(*definition);
  std::vector<std::string> cards;
  std::set<std::string> listed;
  for (const auto& raw : ArrayOr(json::Get(snapshot, "resolution", "[]"))) {
    const auto resolved = ObjectOr(raw);
    if (!resolved) continue;
    const auto name = CardName(json::Get(*resolved, "card"));
    const auto found = definitions.find(name);
    cards.push_back(CardSummary(
        *resolved, found == definitions.end() ? nullptr : &found->second));
    listed.insert(name);
  }
  for (const auto& [id, definition] : definitions)
    if (!listed.contains(id))
      cards.push_back(CardSummary(
          {{"card", json::Encode(json::Members{
                        {"segments", json::Encode(std::vector<std::string>{
                                         json::Quote(id)})}})}},
          &definition));
  out["cards"] = json::Encode(cards);
  out["wires"] = json::Encode(wires);
  std::vector<std::string> files;
  for (const auto& raw : ArrayOr(json::Get(snapshot, "sources", "[]")))
    if (const auto source = ObjectOr(raw);
        source && json::Get(*source, "exists", "true") != "false")
      files.push_back(json::Get(*source, "path"));
  out["files"] = json::Encode(files);
  std::vector<std::string> notes;
  const auto layout = json::Get(snapshot, "layout", "\"\"");
  if (const auto view = ObjectOr(layout.starts_with('"') ? json::String(layout) : layout))
    for (const auto& raw : ArrayOr(json::Get(*view, "notes", "[]")))
      if (const auto note = ObjectOr(raw))
        notes.push_back(json::Get(*note, "text", "\"\""));
  if (!notes.empty()) out["notes"] = json::Encode(notes);
  out["diagnostics"] = json::Get(snapshot, "diagnostics", "[]");
  out["run"] = RunSummary(json::Get(snapshot, "run", "{}"));
  out["detail"] = json::Quote(
      "project_inspect with detail: true adds each definition's _card.json and "
      "README.md, source hashes, resolved ports with Nix references, the "
      "layout and the execution services.");
  return out;
}

auto SaveSummary(const json::Members& save) -> json::Members {
  json::Members out;
  for (const auto& [key, value] : save)
    if (key != "delta" && key != "operation_data") out[key] = value;
  if (json::Get(out, "replayed") != "true") out.erase("replayed");
  if (ArrayOr(json::Get(out, "comments", "[]")).empty()) out.erase("comments");
  const auto delta = ObjectOr(json::Get(save, "delta"));
  if (!delta) {
    out["changed"] = json::Quote("nothing");
    return out;
  }
  if (const auto origin = json::Get(*delta, "origin"); origin != "null")
    out["origin"] = origin;
  json::Members changed;
  std::set<std::string> touched;
  std::vector<std::string> ids;
  for (const auto& raw : ArrayOr(json::Get(*delta, "definitions_upsert", "[]")))
    if (const auto definition = ObjectOr(raw)) {
      ids.push_back(json::Get(*definition, "id"));
      touched.insert(json::Text(*definition, "id"));
    }
  if (!ids.empty()) changed["definitions"] = json::Encode(ids);
  std::vector<std::string> paths;
  for (const auto& raw : ArrayOr(json::Get(*delta, "sources_upsert", "[]")))
    if (const auto source = ObjectOr(raw)) {
      const auto path = json::Text(*source, "path");
      paths.push_back(json::Quote(path));
      for (const auto* prefix : {"definitions/", "externals/"})
        if (path.starts_with(prefix)) {
          const auto rest = path.substr(std::string_view(prefix).size());
          touched.insert(rest.substr(0, rest.find('/')));
        }
    }
  if (!paths.empty()) changed["sources"] = json::Encode(paths);
  for (const auto* field : {"definitions_removed", "sources_removed"})
    if (const auto removed = json::Get(*delta, field, "[]");
        !ArrayOr(removed).empty())
      changed[field] = removed;
  if (json::Get(*delta, "project_json") != "null")
    changed["tsugumi.json"] = "true";
  if (json::Bool(json::Get(*delta, "layout_changed"))) changed["layout"] = "true";
  out["changed"] = changed.empty() ? json::Quote("nothing") : json::Encode(changed);
  std::vector<std::string> diagnostics = ArrayOr(json::Get(*delta, "diagnostics", "[]"));
  std::vector<std::string> cards;
  for (const auto& raw : ArrayOr(json::Get(*delta, "resolution", "[]"))) {
    const auto resolved = ObjectOr(raw);
    if (!resolved) continue;
    const auto name = CardName(json::Get(*resolved, "card"));
    for (const auto& item : ArrayOr(json::Get(*resolved, "diagnostics", "[]")))
      if (auto diagnostic = ObjectOr(item)) {
        (*diagnostic)["card"] = json::Quote(name);
        diagnostics.push_back(json::Encode(*diagnostic));
      }
    if (touched.contains(name.substr(0, name.find('/'))))
      cards.push_back(CardSummary(*resolved, nullptr));
  }
  out["diagnostics"] = json::Encode(diagnostics);
  if (!cards.empty()) out["cards"] = json::Encode(cards);
  return out;
}

auto ModelView(std::string_view tool, const json::Members& arguments,
               std::string text) -> std::string {
  auto envelope = ObjectOr(text);
  if (!envelope || !envelope->contains("reply")) return DecodeRecordedValues(std::move(text));
  const auto reply = json::Text(*envelope, "reply");
  auto value = ObjectOr(json::Get(*envelope, "value", "{}"));
  if (value) {
    if (tool == "project_inspect" && reply == "ProjectSnapshot" &&
        !json::Bool(json::Get(arguments, "detail")))
      *value = InspectSummary(*value);
    else if (reply == "SaveResult")
      *value = SaveSummary(*value);
    if (const auto save = ObjectOr(json::Get(*value, "save")))
      (*value)["save"] = json::Encode(SaveSummary(*save));
    if (json::Get(*envelope, "still_running") == "true") {
      const auto status =
          ObjectOr(tool == "run" ? json::Get(*value, "run", "{}")
                                 : json::Encode(*value));
      const auto wait = json::UInt(json::Get(arguments, "wait_seconds"),
                                   json::UInt(json::Text(arguments, "wait_seconds")));
      (*envelope)["continue"] = json::Encode(json::Members{
          {"tool", json::Quote("run_status")},
          {"arguments",
           json::Encode(json::Members{
               {"run_id", json::Get(status ? *status : json::Members{}, "run_id")},
               {"wait_seconds", std::to_string(wait ? wait : 300)}})}});
    }
    (*envelope)["value"] = json::Encode(*value);
  }
  return DecodeRecordedValues(json::Encode(*envelope));
}

}  // namespace tsugumi::agent
