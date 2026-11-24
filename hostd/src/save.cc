#include "host/save.hh"

#include <algorithm>
#include <set>

#include "host/layout_file.hh"
#include "tsugumi/document/document.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/text.hh"

namespace tsugumi::host {
auto CardStem(const std::string& name) -> std::string {
  if (valid_name(name)) return name;
  std::string stem;
  for (unsigned char c : name) {
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if ((c >= 'a' && c <= 'z') || (!stem.empty() && c >= '0' && c <= '9'))
      stem += static_cast<char>(c);
    else if (!stem.empty() && stem.back() != '_')
      stem += '_';
    if (stem.size() == 64) break;
  }
  if (!stem.empty() && stem.back() == '_') stem.pop_back();
  return stem.empty() ? "card" : stem;
}

namespace json = ::tsugumi::json;

auto FilterProjectGraph(json::Members project,
                        const std::function<bool(const std::string&)>& visible)
    -> json::Members {
  if (project.contains("canvas")) {
    auto canvas = json::Object(project.at("canvas"));
    auto cards = json::Array(json::Get(canvas, "cards", "[]"));
    std::erase_if(cards, [&](const auto& card) {
      return !visible(json::Text(json::Object(card), "id"));
    });
    auto wires = json::Array(json::Get(canvas, "connections", "[]"));
    std::erase_if(wires, [&](const auto& raw) {
      const auto wire = json::Object(raw);
      for (const auto* end : {"from", "to"})
        if (!visible(
                json::Text(json::Object(json::Get(wire, end, "{}")), "card")))
          return true;
      return false;
    });
    canvas["cards"] = json::Encode(cards);
    canvas["connections"] = json::Encode(wires);
    project["canvas"] = json::Encode(canvas);
  }
  if (project.contains("boundary")) {
    auto boundary = json::Object(project.at("boundary"));
    for (auto& [side, raw] : boundary) {
      (void)side;
      auto ports = json::Array(raw);
      std::erase_if(ports, [&](const auto& port) {
        return !visible(json::Text(json::Object(port), "card"));
      });
      raw = json::Encode(ports);
    }
    project["boundary"] = json::Encode(boundary);
  }
  return project;
}

auto DecodeSourceBytes(const std::string& raw) -> std::string {
  if (raw.starts_with('"')) return json::String(raw);
  if (raw.starts_with('{')) {
    std::string out;
    if (!crypto::base64_decode(json::Text(json::Object(raw), "base64"), out))
      throw std::runtime_error("Invalid source encoding");
    return out;
  }
  std::string out;
  for (const auto& n : json::Array(raw))
    out += static_cast<char>(json::UInt(n));
  return out;
}
auto SourcePatches::Project(std::string_view source,
                            const std::vector<std::string>& patches) const
    -> const std::string& {
  auto key = std::pair{crypto::sha256_hex(source), patches};
  if (const auto found = projected_.find(key); found != projected_.end())
    return found->second;
  text::PatchApplier apply(source);
  for (const auto& raw : patches) {
    const auto patch = json::Object(raw);
    const auto integer = [&](const char* field) {
      const auto value = json::Get(patch, field);
      return json::UInt(json::String(value, value), UINT64_MAX);
    };
    apply.append(integer("offset"), integer("remove"),
                 json::Text(patch, "text"));
  }
  return projected_.emplace(std::move(key), std::move(apply).finish())
      .first->second;
}

auto CaptureSaveBase(const json::Members& snapshot) -> SaveBase {
  SaveBase base;
  for (const auto* key : {"rev", "project_json", "layout", "definitions",
                          "sources", "resolution"})
    if (snapshot.contains(key)) base.snapshot[key] = snapshot.at(key);
  base.files["tsugumi.json"] = json::Text(snapshot, "project_json", "{}");
  for (const auto& raw :
       json::Array(json::Get(snapshot, "definitions", "[]"))) {
    const auto definition = json::Object(raw);
    for (const auto& [key, file] :
         {std::pair{"card_json", "_card.json"},
          {"readme", "README.md"},
          {"comments_json", "_comments.json"}}) {
      const auto text = json::Text(definition, key);
      if (!text.empty())
        base.files["definitions/" + json::Text(definition, "id") + '/' + file] =
            text;
    }
  }
  // Each project's layout.bin and notes/<id>.md are one JSON view.
  std::map<std::string, std::string> layouts;  // prefix -> layout.bin
  std::map<std::string, std::map<std::string, std::string>> notes;
  layouts[""] = DecodeSourceBytes(json::Get(snapshot, "layout", "\"\""));
  for (const auto& raw : json::Array(json::Get(snapshot, "sources", "[]"))) {
    const auto source = json::Object(raw);
    const auto path = json::Text(source, "path");
    if (!source.contains("document")) continue;
    const auto bytes = DecodeSourceBytes(source.at("document"));
    const auto notes_at = path.rfind("notes/");
    if (Document::is_note_file(path) && notes_at != std::string::npos) {
      const auto prefix = path.substr(0, notes_at);
      if (const auto id = NoteId(prefix, path)) notes[prefix][*id] = bytes;
    } else if (path.ends_with("layout.bin") &&
               Document::is_external_document_file(path)) {
      layouts[path.substr(0, path.size() - 10)] = bytes;
    } else {
      base.files[path] = bytes;
    }
  }
  for (const auto& [prefix, _] : notes) layouts.try_emplace(prefix);
  for (const auto& [prefix, layout] : layouts) {
    if (!prefix.empty() && layout.empty() && !notes.contains(prefix)) continue;
    try {
      base.files[prefix + "layout.bin"] = LayoutView(layout, notes[prefix]);
    } catch (const std::exception&) {
      base.files[prefix + "layout.bin"] = "{}";
    }
  }
  return base;
}

}  // namespace tsugumi::host
