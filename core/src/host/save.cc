#include "host/save.hh"

#include <algorithm>
#include <set>

#include "host/comments.hh"
#include "host/document_edits.hh"
#include "host/failure.hh"
#include "host/layout_saving.hh"
#include "util/crypto.hh"

namespace tsugumi::host {
auto CardStem(const std::string& name) -> std::string {
  std::string stem;
  for (unsigned char c : name) {
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
      stem += static_cast<char>(c);
    else if (!stem.empty() && stem.back() != '-')
      stem += '-';
    if (stem.size() == 123) break;
  }
  while (!stem.empty() && stem.back() == '-') stem.pop_back();
  return stem.empty() ? "card" : stem;
}

namespace {
namespace json = agent::json;
auto Content(const std::string& raw) -> std::string {
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
auto DocumentPath(const std::string& path) -> bool {
  if (path == "tsugumi.json" || path == "layout.json" ||
      IsExternalDocument(path, "tsugumi.json") ||
      IsExternalDocument(path, "layout.json"))
    return true;
  return (path.ends_with("/_card.json") || path.ends_with("/_meta.json") ||
          path.ends_with("/_link.json")) &&
         comments::is_meta(path.substr(0, path.size() - 10) + "_meta.json");
}
auto Hashes(const SaveBase& base) -> json::Members {
  json::Members out;
  for (const auto& raw :
       json::Array(json::Get(base.snapshot, "sources", "[]"))) {
    const auto source = json::Object(raw);
    out[json::Text(source, "path")] = json::Get(source, "hash");
  }
  return out;
}
auto Same(const std::string& a, const std::string& b) -> bool {
  return DocumentsEqual(a, b);
}
}  // namespace

auto CaptureSaveBase(const json::Members& snapshot, const SaveRead& read)
    -> SaveBase {
  SaveBase base;
  for (const auto* key :
       {"rev", "project_json", "layout_json", "definitions", "sources"})
    if (snapshot.contains(key)) base.snapshot[key] = snapshot.at(key);
  base.files["tsugumi.json"] = json::Text(snapshot, "project_json", "{}");
  base.files["layout.json"] = json::Text(snapshot, "layout_json", "{}");
  for (const auto& raw :
       json::Array(json::Get(snapshot, "definitions", "[]"))) {
    const auto definition = json::Object(raw);
    for (const auto& [key, file] : {std::pair{"card_json", "_card.json"},
                                    {"meta_json", "_meta.json"},
                                    {"link_json", "_link.json"}}) {
      const auto text = json::Text(definition, key);
      if (!text.empty())
        base.files["definitions/" + json::Text(definition, "id") + '/' + file] =
            text;
    }
  }
  for (const auto& raw : json::Array(json::Get(snapshot, "sources", "[]"))) {
    const auto source = json::Object(raw);
    const auto path = json::Text(source, "path");
    if (!DocumentPath(path)) continue;
    std::string text;
    for (;;) {
      const auto value =
          read("project_file_read", {{"path", json::Quote(path)},
                                     {"at_rev", json::Get(snapshot, "rev")},
                                     {"if_hash", json::Get(source, "hash")},
                                     {"offset", std::to_string(text.size())},
                                     {"limit_bytes", "1048576"}});
      const auto page = Content(json::Get(value, "bytes"));
      text += page;
      if (json::Bool(json::Get(value, "eof"))) break;
      if (page.empty())
        throw std::runtime_error("Document read made no progress: " + path);
    }
    base.files[path] = std::move(text);
  }
  return base;
}

auto InspectForSave(const SaveRead& read) -> SaveInspection {
  for (int attempt = 0; attempt < 8; ++attempt) {
    try {
      auto snapshot = read("project_inspect", {});
      auto base = CaptureSaveBase(snapshot, read);
      return {std::move(snapshot), std::move(base)};
    } catch (const Failure& error) {
      if (error.code != "revision_conflict") throw;
    }
  }
  throw Failure("revision_conflict",
                "Project kept changing during inspection; inspect again");
}

auto AdvanceSaveBase(const SaveBase& base, const json::Members& result,
                     const json::Members& arguments) -> SaveBase {
  if (!result.contains("delta") || result.at("delta") == "null") return base;
  const auto delta = json::Object(result.at("delta"));
  auto snapshot = base.snapshot;
  for (const auto* key : {"rev", "project_json", "layout_json"})
    if (delta.contains(key)) snapshot[key] = delta.at(key);
  auto update = [&](const char* field, const char* upsert, const char* removed,
                    const char* key) {
    json::Members entries;
    for (const auto& raw : json::Array(json::Get(snapshot, field, "[]")))
      entries[json::Text(json::Object(raw), key)] = raw;
    for (const auto& raw : json::Array(json::Get(delta, removed, "[]")))
      entries.erase(json::String(raw));
    for (const auto& raw : json::Array(json::Get(delta, upsert, "[]")))
      entries[json::Text(json::Object(raw), key)] = raw;
    std::vector<std::string> values;
    for (const auto& [id, raw] : entries) values.push_back(raw);
    snapshot[field] = json::Encode(values);
  };
  update("definitions", "definitions_upsert", "definitions_removed", "id");
  update("sources", "sources_upsert", "sources_removed", "path");
  auto files = base.files;
  for (const auto& raw : json::Array(json::Get(arguments, "changes", "[]"))) {
    const auto change = json::Object(raw);
    const auto kind = json::Text(change, "value_type");
    const auto value = json::Object(json::Get(change, "value", "{}"));
    const auto path = json::Text(value, "path");
    if (!DocumentPath(path)) continue;
    if (kind == "PutSource")
      files[path] = Content(json::Get(value, "bytes"));
    else if (kind == "PatchSource") {
      const auto previous = files.at(path);
      std::string text;
      size_t cursor = 0;
      for (const auto& raw_patch : json::Array(json::Get(value, "patches"))) {
        const auto patch = json::Object(raw_patch);
        auto integer = [&](const char* key) {
          const auto raw = json::Get(patch, key);
          return json::UInt(json::String(raw, raw));
        };
        const auto offset = integer("offset"), remove = integer("remove");
        text += previous.substr(cursor, offset - cursor) +
                json::Text(patch, "text");
        cursor = offset + remove;
      }
      files[path] = text + previous.substr(cursor);
    } else if (kind == "RemoveSource")
      files.erase(path);
  }
  // Definition and root documents come from the acknowledged delta. External
  // document bytes come from the acknowledged request, never a post-save read.
  for (const auto* key : {"project_json", "layout_json"})
    if (delta.contains(key))
      files[std::string(key) == "project_json" ? "tsugumi.json"
                                               : "layout.json"] =
          json::Text(delta, key);
  for (const auto& raw :
       json::Array(json::Get(delta, "definitions_upsert", "[]"))) {
    const auto definition = json::Object(raw);
    const auto dir = "definitions/" + json::Text(definition, "id") + '/';
    for (const auto& [key, name] : {std::pair{"card_json", "_card.json"},
                                    {"meta_json", "_meta.json"},
                                    {"link_json", "_link.json"}}) {
      files.erase(dir + name);
      const auto text = json::Text(definition, key);
      if (!text.empty()) files[dir + name] = text;
    }
  }
  return CaptureSaveBase(snapshot, [&](const auto&, const auto& query) {
    return json::Members{
        {"bytes", json::Quote(files.at(json::Text(query, "path")))},
        {"eof", "true"}};
  });
}

auto RebaseSave(json::Members args, const SaveBase& base,
                const SaveBase& latest) -> json::Members {
  auto ours = base.files;
  auto changes = json::Array(json::Get(args, "changes", "[]"));
  std::set<std::string> written_sources;
  const auto hashes = Hashes(base), current_hashes = Hashes(latest);
  auto get = [](const json::Members& values, const std::string& key) {
    return json::Get(values, key, "null");
  };
  auto conflict = [](const std::string& path) {
    throw std::runtime_error("Concurrent edit conflicts at " + path +
                             "; inspect and re-apply");
  };
  for (const auto& raw : changes) {
    const auto c = json::Object(raw);
    const auto kind = json::Text(c, "value_type");
    const auto value = json::Object(json::Get(c, "value", "{}"));
    const auto id = json::Text(value, "id"), path = json::Text(value, "path");
    if (kind == "SetProject" || kind == "SetLayout") {
      ours[kind == "SetProject" ? "tsugumi.json" : "layout.json"] =
          json::Text(value, "json");
    } else if (kind == "PutDefinition") {
      const auto prefix = "definitions/" + id + '/';
      ours.erase(prefix + "_card.json");
      ours.erase(prefix + "_link.json");
      const auto link = json::Text(value, "link_json");
      ours[prefix + (link.empty() ? "_card.json" : "_link.json")] =
          link.empty() ? json::Text(value, "card_json") : link;
      const auto meta = json::Text(value, "meta_json");
      if (!meta.empty())
        ours[prefix + "_meta.json"] = meta;
      else if (!json::Bool(json::Get(value, "keep_meta")) &&
               !value.contains("meta_json"))
        ours.erase(prefix + "_meta.json");
    } else if (kind == "RemoveDefinition") {
      const auto prefix = "definitions/" + id + '/';
      std::erase_if(ours, [&](const auto& entry) {
        return entry.first.starts_with(prefix);
      });
      // Removing a definition removes its source folder as well.
      for (const auto& [p, hash] : current_hashes)
        if (p.starts_with(prefix) && get(hashes, p) != hash) conflict(p);
    } else if (kind == "PutSource" || kind == "PatchSource" ||
               kind == "RemoveSource") {
      written_sources.insert(path);
      if (json::Get(value, "base_hash", "null") != get(hashes, path))
        conflict(path + " (base_hash)");
      if (!DocumentPath(path)) {
        if (get(hashes, path) != get(current_hashes, path)) conflict(path);
        continue;
      }
      if (kind == "RemoveSource")
        ours.erase(path);
      else if (kind == "PutSource")
        ours[path] = Content(json::Get(value, "bytes"));
      else {
        const auto previous = base.files.at(path);
        std::string text;
        size_t offset = 0;
        for (const auto& raw_patch :
             json::Array(json::Get(value, "patches", "[]"))) {
          const auto patch = json::Object(raw_patch);
          const auto start = json::UInt(json::String(
              json::Get(patch, "offset"), json::Get(patch, "offset")));
          const auto count = json::UInt(json::String(
              json::Get(patch, "remove"), json::Get(patch, "remove")));
          if (start < offset || start > previous.size() ||
              count > previous.size() - start)
            throw std::runtime_error("Invalid patch range: " + path);
          auto boundary = [&](size_t at) {
            return at == previous.size() ||
                   (static_cast<unsigned char>(previous[at]) & 0xc0) != 0x80;
          };
          if (!boundary(start) || !boundary(start + count))
            throw std::runtime_error("Patch splits UTF-8: " + path);
          text += previous.substr(offset, start - offset) +
                  json::Text(patch, "text");
          offset = start + count;
        }
        ours[path] = text + previous.substr(offset);
      }
    }
  }
  for (const auto& [path, text] : base.files) {
    if (path != "tsugumi.json" && !IsExternalDocument(path, "tsugumi.json"))
      continue;
    const auto incoming = [](const std::string& document) {
      json::Members out;
      const auto canvas =
          json::Object(json::Get(json::Object(document), "canvas", "{}"));
      for (const auto& raw :
           json::Array(json::Get(canvas, "connections", "[]"))) {
        const auto wire = json::Object(raw);
        const auto endpoint = json::Object(json::Get(wire, "to", "{}"));
        const auto key =
            json::Text(endpoint, "card") + ":" + json::Text(endpoint, "port");
        auto list = json::Array(json::Get(out, key, "[]"));
        list.push_back(raw);
        out[key] = json::Encode(list);
      }
      return out;
    };
    const auto bi = incoming(text), oi = incoming(json::Get(ours, path, "{}")),
               ti = incoming(json::Get(latest.files, path, "{}"));
    auto keys = oi;
    keys.insert(ti.begin(), ti.end());
    for (const auto& [key, ignored] : keys) {
      (void)ignored;
      const auto b = json::Get(bi, key, "[]"), o = json::Get(oi, key, "[]"),
                 t = json::Get(ti, key, "[]");
      if (!Same(b, o) && !Same(b, t) && !Same(o, t))
        conflict(path + "/canvas/connections input " + key);
    }
  }
  // Wrap by file path so every document, including nested layout, uses the
  // same strict merge. Every collision is reported.
  const auto merge = MergeDocuments(
      json::Encode(base.files), json::Encode(ours), json::Encode(latest.files));
  if (!merge.conflicts.empty()) {
    std::string message;
    for (const auto& path : merge.conflicts)
      message += (message.empty() ? "" : ", ") + path;
    conflict(message);
  }
  const auto merged = json::Object(merge.merged_json);
  // Cross-file delete/change conflicts are semantic: Card membership is in
  // tsugumi.json, while the changed definition is in another document.
  for (const auto& [path, raw] : base.files) {
    if (path != "tsugumi.json" && !IsExternalDocument(path, "tsugumi.json"))
      continue;
    const auto ids = [](const std::string& project) {
      std::set<std::string> out;
      const auto graph =
          json::Object(json::Get(json::Object(project), "canvas", "{}"));
      for (const auto& c : json::Array(json::Get(graph, "cards", "[]")))
        out.insert(json::Text(json::Object(c), "id"));
      return out;
    };
    const auto prefix = path.substr(0, path.size() - 12);
    const auto before = ids(raw);
    const auto oi = ids(json::Get(ours, path, "{}")),
               ti = ids(json::Get(latest.files, path, "{}"));
    for (const auto& id : before) {
      const auto dir = prefix + "definitions/" + id + '/';
      if (!oi.contains(id) && ti.contains(id)) {
        auto paths = hashes;
        paths.insert(current_hashes.begin(), current_hashes.end());
        for (const auto& [p, ignored] : paths) {
          (void)ignored;
          if (p.starts_with(dir) && get(hashes, p) != get(current_hashes, p))
            conflict(p + " (removed Card changed)");
        }
      }
      if (!ti.contains(id) && oi.contains(id))
        for (const auto& p : written_sources)
          if (p.starts_with(dir)) conflict(p + " (Card removed)");

      for (const auto& [removed, edited] :
           {std::pair{&oi, &latest.files},
            std::pair{&ti, static_cast<const json::Members*>(&ours)}}) {
        if (removed->contains(id)) continue;
        const auto dir = prefix + "definitions/" + id + '/';
        for (const auto& [p, v] : *edited)
          if (p.starts_with(dir) &&
              (!base.files.contains(p) || !Same(base.files.at(p), v)))
            conflict(dir);
      }
    }
    if (merged.contains(path)) {
      const auto members = ids(merged.at(path));
      const auto graph = json::Object(
          json::Get(json::Object(merged.at(path)), "canvas", "{}"));
      for (const auto& w : json::Array(json::Get(graph, "connections", "[]")))
        for (const auto* side : {"from", "to"}) {
          const auto id = json::Text(
              json::Object(json::Get(json::Object(w), side, "{}")), "card");
          if (!members.contains(id))
            conflict(path + "/canvas/cards/" + id + " (removed Card is wired)");
        }
    }
  }
  std::vector<std::string> rebased;
  for (auto raw : changes) {
    auto c = json::Object(raw);
    const auto kind = json::Text(c, "value_type");
    auto value = json::Object(json::Get(c, "value", "{}"));
    const auto path = json::Text(value, "path"), id = json::Text(value, "id");
    if (kind == "SetProject" || kind == "SetLayout")
      value["json"] = json::Quote(
          merged.at(kind == "SetProject" ? "tsugumi.json" : "layout.json"));
    else if (kind == "PutDefinition") {
      const auto dir = "definitions/" + id + '/';
      for (const auto& [key, file] : {std::pair{"card_json", "_card.json"},
                                      {"link_json", "_link.json"},
                                      {"meta_json", "_meta.json"}}) {
        value.erase(key);
        if (merged.contains(dir + file))
          value[key] = json::Quote(merged.at(dir + file));
      }
      // The wire field is required even for a link, which carries an empty
      // string (a nonempty Card document and link_json are mutually exclusive).
      if (!value.contains("card_json")) value["card_json"] = json::Quote("");
      value.erase("keep_meta");
    } else if ((kind == "PutSource" || kind == "PatchSource") &&
               DocumentPath(path)) {
      c["value_type"] = json::Quote("PutSource");
      value.erase("patches");
      value["bytes"] = json::Quote(merged.at(path));
      value["base_hash"] = get(current_hashes, path);
    } else if (kind == "RemoveSource" && DocumentPath(path))
      value["base_hash"] = get(current_hashes, path);
    c["value"] = json::Encode(value);
    rebased.push_back(json::Encode(c));
  }
  args["changes"] = json::Encode(rebased);
  args["base_rev"] = json::Get(latest.snapshot, "rev");
  return args;
}
auto SaveWithMerge(
    const json::Members &original, const SaveBase &base, const SaveRead &read,
    const std::function<json::Members(const json::Members &)> &send)
    -> json::Members {
  for (int attempt = 0; attempt < 8; ++attempt) {
    const auto latest = InspectForSave(read).base;
    auto args = original;
    try {
      // External documents can finish loading behind a newer root snapshot.
      // Merge their actual read values even when the root revision matches.
      args = RebaseSave(args, base, latest);
    } catch (const std::exception &error) {
      throw Failure("revision_conflict", error.what());
    }
    // A refused CAS has a durable core receipt; never reuse its operation id.
    args["op_id"] = json::Quote(crypto::uuid4());
    const auto result = send(args);
    const auto value = json::Object(json::Get(result, "value", "{}"));
    if (json::Text(result, "reply") != "Failure" ||
        json::Text(value, "kind") != "revision_conflict")
      return result;
  }
  throw Failure("revision_conflict",
                "Project kept changing during save; your edit is retained");
}
}  // namespace tsugumi::host
