#include "agent/save_policy.hh"

#include <set>

#include "host/failure.hh"
#include "tsugumi/util/crypto.hh"

namespace tsugumi::agent {
namespace {
namespace comments = host::comments;
auto Blank(const std::string& text) -> bool {
  return text.find_first_not_of(" \t\r\n") == std::string::npos;
}
auto Object(const std::string& text) -> json::Members {
  try {
    return json::Object(text);
  } catch (const std::exception&) {
    return {};
  }
}
auto Bytes(const std::string& raw) -> std::string {
  if (raw.starts_with('"')) return json::String(raw);
  if (raw.starts_with('{')) {
    std::string out;
    if (!crypto::base64_decode(json::Text(json::Object(raw), "base64"), out))
      throw std::runtime_error("Invalid base64");
    return out;
  }
  std::string out;
  for (const auto& value : json::Array(raw))
    out += static_cast<char>(json::UInt(value));
  return out;
}
}  // namespace

auto PrepareAgentSave(json::Members args, const host::SaveBase& base,
                      const host::SaveRead& read,
                      const host::comments::ReserveComment& reserve,
                      const host::SourcePatches& patches) -> AgentSave {
  const auto rev = json::Get(base.snapshot, "rev");
  auto before = base.files;
  std::map<std::string, std::string> hashes;
  for (const auto& raw :
       json::Array(json::Get(base.snapshot, "sources", "[]"))) {
    const auto source = json::Object(raw);
    hashes[json::Text(source, "path")] = json::Text(source, "hash");
  }
  auto after = before;
  auto encoded_changes = json::Array(json::Get(args, "changes", "[]"));
  struct Change {
    json::Members wrapper;
    json::Members value;
    std::string kind;
  };
  std::vector<Change> changes;
  changes.reserve(encoded_changes.size());
  std::set<std::string> changed;
  for (const auto& raw : encoded_changes) {
    auto wrapper = json::Object(raw);
    auto kind = json::Text(wrapper, "value_type");
    auto c = json::Object(json::Get(wrapper, "value", "{}"));
    const auto id = json::Text(c, "id"), path = json::Text(c, "path");
    if (kind == "PutDefinition") {
      const auto dir = "definitions/" + id + "/";
      after[dir + "_card.json"] = json::Text(c, "card_json");
      if (const auto meta = json::Text(c, "meta_json"); !meta.empty())
        after[dir + "_meta.json"] = meta;
      else if (!(json::Bool(json::Get(c, "keep_meta")) ||
                 json::Get(c, "meta_json") == "\"\"" ||
                 c.contains("fill_description") || c.contains("fill_context")))
        after.erase(dir + "_meta.json");
      auto meta = Object(after.contains(dir + "_meta.json")
                             ? after.at(dir + "_meta.json")
                             : "{}");
      bool filled = false;
      for (const auto& field : {"description", "context"}) {
        const auto fill = json::Text(c, "fill_" + std::string(field));
        if (!fill.empty() && Blank(json::Text(meta, field))) {
          meta[field] = json::Quote(fill);
          filled = true;
        }
        c.erase("fill_" + std::string(field));
      }
      if (filled) after[dir + "_meta.json"] = json::Encode(meta);
    } else if (kind == "RemoveDefinition") {
      const auto dir = "definitions/" + id + "/";
      std::erase_if(
          after, [&](const auto& item) { return item.first.starts_with(dir); });
      changed.insert(id);
    } else if (kind == "PutSource" || kind == "PatchSource" ||
               kind == "RemoveSource") {
      if (kind == "RemoveSource")
        after.erase(path);
      else {
        std::string value;
        if (kind == "PutSource")
          value = Bytes(json::Get(c, "bytes"));
        else {
          const auto old =
              before.contains(path)
                  ? before.at(path)
                  : Bytes(json::Get(
                        read("project_file_read",
                             {{"path", json::Quote(path)}, {"at_rev", rev}}),
                        "bytes"));
          before[path] = old;
          value =
              patches.Project(old, json::Array(json::Get(c, "patches", "[]")));
        }
        after[path] = value;
      }
      if (!comments::is_meta(path))
        if (auto card = comments::card_of(path, after);
            card && (kind == "RemoveSource" || !hashes.contains(path) ||
                     hashes.at(path) != crypto::sha256_hex(after.at(path))))
          changed.insert(*card);
    }
    changes.push_back({std::move(wrapper), std::move(c), std::move(kind)});
  }
  // A Draft's intent is Agent policy, checked before core permission approvals.
  for (const auto& [path, text] : before) {
    if (!path.ends_with("/_card.json") ||
        json::Text(Object(text), "template") != "draft")
      continue;
    const bool implemented =
        after.contains(path) &&
        json::Text(Object(after.at(path)), "template") != "draft";
    if (!implemented) continue;
    auto meta_path = path.substr(0, path.size() - 10) + "_meta.json";
    auto old_meta =
        Object(before.contains(meta_path) ? before.at(meta_path) : "{}");
    auto new_meta =
        Object(after.contains(meta_path) ? after.at(meta_path) : "{}");
    auto fail = [&](const std::string& why) {
      throw host::Failure(
          "validation_failed",
          comments::card_of(path, after).value_or(path) + ": " + why);
    };
    for (const auto* field : {"description", "content", "context"})
      if (!Blank(json::Text(old_meta, field)) &&
          json::Text(old_meta, field) != json::Text(new_meta, field))
        fail(std::string(field) +
             " changed; send what the Draft had, word for word");
    if (Blank(json::Text(new_meta, "description")))
      fail(
          "description is empty; write a one-line summary of what the Card "
          "does");
    if (Blank(json::Text(new_meta, "context")) &&
        !(Blank(json::Text(old_meta, "description")) &&
          Blank(json::Text(old_meta, "content"))))
      fail(
          "_meta.json context is empty; write context as what the user asked "
          "of this Card, preserving its existing description and content");
  }
  for (const auto& [path, text] : after)
    if (!comments::is_meta(path) &&
        (!before.contains(path) || before.at(path) != text))
      if (auto card = comments::card_of(path, after)) changed.insert(*card);
  const auto was = comments::of(before);
  const std::vector<std::string> cards(changed.begin(), changed.end());
  // Stamp only metadata written by this save. A keep-only save preserves it.
  for (auto& [wrapper, c, kind] : changes) {
    const auto path = kind == "PutDefinition"
                          ? "definitions/" + json::Text(c, "id") + "/_meta.json"
                          : json::Text(c, "path");
    if ((kind == "PutDefinition" || kind == "PutSource" ||
         kind == "PatchSource") &&
        comments::is_meta(path) && after.contains(path)) {
      const auto original = after.at(path);
      after[path] = comments::stamped(
          original, comments::card_of(path, after).value_or(path), was, cards,
          reserve);
      if (kind == "PutDefinition") {
        c["meta_json"] = json::Quote(after.at(path));
        c.erase("keep_meta");
      } else if (after.at(path) != original) {
        wrapper["value_type"] = json::Quote("PutSource");
        c.erase("patches");
        c["bytes"] = json::Quote(after.at(path));
      }
    }
  }
  encoded_changes.clear();
  for (auto& change : changes) {
    change.wrapper["value"] = json::Encode(change.value);
    encoded_changes.push_back(json::Encode(change.wrapper));
  }
  args["changes"] = json::Encode(encoded_changes);
  return {std::move(args),
          comments::record_json(comments::changes(was, comments::of(after)))};
}
}  // namespace tsugumi::agent
