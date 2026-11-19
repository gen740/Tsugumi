#include "host/save.hh"

#include <algorithm>
#include <set>

#include "host/comments.hh"
#include "host/document_edits.hh"
#include "host/failure.hh"
#include "host/layout_saving.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/text.hh"

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

namespace json = ::tsugumi::json;
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
namespace {
auto DocumentPath(const std::string& path) -> bool {
  if (path == "tsugumi.json" || path == "layout.json" ||
      IsExternalDocument(path, "tsugumi.json") ||
      IsExternalDocument(path, "layout.json"))
    return true;
  return (path.ends_with("/_card.json") || path.ends_with("/_meta.json")) &&
         comments::is_meta(path.substr(0, path.size() - 10) + "_meta.json");
}
}  // namespace

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
    for (const auto& [key, file] :
         {std::pair{"card_json", "_card.json"}, {"meta_json", "_meta.json"}}) {
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
      const auto page = DecodeSourceBytes(json::Get(value, "bytes"));
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

}  // namespace tsugumi::host
