#include "host/save.hh"

#include "util/crypto.hh"
#include <set>

namespace tsugumi::host {
namespace {
namespace json = agent::json;
// Project RPC exposes 64-bit integers as decimal strings to the Agent.
auto Integer(const std::string &raw) -> uint64_t {
  return json::UInt(json::String(raw, raw));
}
auto Blank(const std::string &text) -> bool {
  return text.find_first_not_of(" \t\r\n") == std::string::npos;
}
auto Object(const std::string &text) -> json::Members {
  try {
    return json::Object(text);
  } catch (const std::exception &) {
    return {};
  }
}
auto DefinitionFile(const std::string &path) -> bool {
  if (path.size() < 10)
    return false;
  if (!path.ends_with("/_card.json") && !path.ends_with("/_meta.json") &&
      !path.ends_with("/_link.json"))
    return false;
  return comments::is_meta(path.substr(0, path.size() - 10) + "_meta.json");
}
auto Bytes(const std::string &raw) -> std::string {
  if (raw.starts_with('"'))
    return json::String(raw);
  if (raw.starts_with('{')) {
    std::string out;
    if (!crypto::base64_decode(json::Text(json::Object(raw), "base64"), out))
      throw std::runtime_error("Invalid base64");
    return out;
  }
  std::string out;
  for (const auto &value : json::Array(raw))
    out += static_cast<char>(json::UInt(value));
  return out;
}
} // namespace

auto CardStem(const std::string &name) -> std::string {
  std::string stem;
  for (unsigned char c : name) {
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
      stem += static_cast<char>(c);
    else if (!stem.empty() && stem.back() != '-')
      stem += '-';
    if (stem.size() == 123)
      break;
  }
  while (!stem.empty() && stem.back() == '-')
    stem.pop_back();
  return stem.empty() ? "card" : stem;
}

auto PrepareAgentSave(json::Members args, const ProjectRead &read)
    -> AgentSave {
  const auto snapshot = read("project_inspect", {});
  if (Integer(json::Get(snapshot, "rev")) !=
      Integer(json::Get(args, "base_rev")))
    throw SaveRevisionConflict("The document changed since base_rev");
  const auto rev = json::Get(snapshot, "rev");
  comments::Files before;
  std::map<std::string, std::string> hashes;
  for (const auto &raw :
       json::Array(json::Get(snapshot, "definitions", "[]"))) {
    auto d = json::Object(raw);
    auto dir = "definitions/" + json::Text(d, "id") + "/";
    for (const auto &[field, file] : {std::pair{"card_json", "_card.json"},
                                      {"link_json", "_link.json"},
                                      {"meta_json", "_meta.json"}})
      if (!json::Text(d, field).empty())
        before[dir + file] = json::Text(d, field);
  }
  for (const auto &raw : json::Array(json::Get(snapshot, "sources", "[]"))) {
    const auto source = json::Object(raw);
    const auto path = json::Text(source, "path");
    hashes[path] = json::Text(source, "hash");
    if (path.starts_with("externals/") && DefinitionFile(path))
      before[path] =
          Bytes(json::Get(read("project_file_read",
                               {{"path", json::Quote(path)}, {"at_rev", rev}}),
                          "bytes"));
  }
  auto after = before;
  auto changes = json::Array(json::Get(args, "changes", "[]"));
  std::set<std::string> changed;
  for (auto &raw : changes) {
    auto wrapper = json::Object(raw);
    const auto kind = json::Text(wrapper, "value_type");
    auto c = json::Object(json::Get(wrapper, "value", "{}"));
    const auto id = json::Text(c, "id"), path = json::Text(c, "path");
    if (kind == "PutDefinition") {
      const auto dir = "definitions/" + id + "/";
      after.erase(dir + "_card.json");
      after.erase(dir + "_link.json");
      const auto link = json::Text(c, "link_json");
      after[dir + (link.empty() ? "_card.json" : "_link.json")] =
          link.empty() ? json::Text(c, "card_json") : link;
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
      for (const auto &field : {"description", "context"}) {
        const auto fill = json::Text(c, "fill_" + std::string(field));
        if (!fill.empty() && Blank(json::Text(meta, field))) {
          meta[field] = json::Quote(fill);
          filled = true;
        }
        c.erase("fill_" + std::string(field));
      }
      if (filled)
        after[dir + "_meta.json"] = json::Encode(meta);
    } else if (kind == "RemoveDefinition") {
      const auto dir = "definitions/" + id + "/";
      std::erase_if(
          after, [&](const auto &item) { return item.first.starts_with(dir); });
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
          size_t offset = 0;
          for (const auto &patch : json::Array(json::Get(c, "patches", "[]"))) {
            auto p = json::Object(patch);
            const auto start = Integer(json::Get(p, "offset")),
                       end = start + Integer(json::Get(p, "remove"));
            if (start < offset || end < start || end > old.size())
              throw std::runtime_error("Invalid patch range");
            value += old.substr(offset, start - offset) + json::Text(p, "text");
            offset = end;
          }
          value += old.substr(offset);
        }
        after[path] = value;
      }
      if (!comments::is_meta(path))
        if (auto card = comments::card_of(path, after);
            card && (kind == "RemoveSource" || !hashes.contains(path) ||
                     hashes.at(path) != crypto::sha256_hex(after.at(path))))
          changed.insert(*card);
    }
    wrapper["value"] = json::Encode(c);
    raw = json::Encode(wrapper);
  }
  // A Draft's intent is Agent policy, checked before core permission approvals.
  for (const auto &[path, text] : before) {
    if (!path.ends_with("/_card.json") ||
        json::Text(Object(text), "template") != "draft")
      continue;
    const auto link_path = path.substr(0, path.size() - 10) + "_link.json";
    const bool implemented =
        after.contains(link_path) ||
        (after.contains(path) &&
         json::Text(Object(after.at(path)), "template") != "draft");
    if (!implemented)
      continue;
    auto meta_path = path.substr(0, path.size() - 10) + "_meta.json";
    auto old_meta =
        Object(before.contains(meta_path) ? before.at(meta_path) : "{}");
    auto new_meta =
        Object(after.contains(meta_path) ? after.at(meta_path) : "{}");
    auto fail = [&](const std::string &why) {
      throw std::runtime_error(comments::card_of(path, after).value_or(path) +
                               ": " + why);
    };
    for (const auto *field : {"description", "content", "context"})
      if (!Blank(json::Text(old_meta, field)) &&
          json::Text(old_meta, field) != json::Text(new_meta, field))
        fail(std::string(field) +
             " changed; send what the Draft had, word for word");
    if (Blank(json::Text(new_meta, "description")))
      fail("description is empty; write a one-line summary of what the Card "
           "does");
    if (Blank(json::Text(new_meta, "context")) &&
        !(Blank(json::Text(old_meta, "description")) &&
          Blank(json::Text(old_meta, "content"))))
      fail("context is empty; write what the user asked of this Card");
  }
  for (const auto &[path, text] : after)
    if (!comments::is_meta(path) &&
        (!before.contains(path) || before.at(path) != text))
      if (auto card = comments::card_of(path, after))
        changed.insert(*card);
  const auto was = comments::of(before);
  const std::vector<std::string> cards(changed.begin(), changed.end());
  // Stamp only metadata written by this save. A keep-only save preserves it.
  for (auto &raw : changes) {
    auto wrapper = json::Object(raw),
         c = json::Object(json::Get(wrapper, "value", "{}"));
    const auto kind = json::Text(wrapper, "value_type");
    const auto path = kind == "PutDefinition"
                          ? "definitions/" + json::Text(c, "id") + "/_meta.json"
                          : json::Text(c, "path");
    if ((kind == "PutDefinition" || kind == "PutSource" ||
         kind == "PatchSource") &&
        comments::is_meta(path) && after.contains(path)) {
      const auto original = after.at(path);
      after[path] = comments::stamped(
          original, comments::card_of(path, after).value_or(path), was, cards);
      if (kind == "PutDefinition") {
        c["meta_json"] = json::Quote(after.at(path));
        c.erase("keep_meta");
      } else if (after.at(path) != original) {
        wrapper["value_type"] = json::Quote("PutSource");
        c.erase("patches");
        c["bytes"] = json::Quote(after.at(path));
      }
    }
    wrapper["value"] = json::Encode(c);
    raw = json::Encode(wrapper);
  }
  args["changes"] = json::Encode(changes);
  return {std::move(args),
          comments::record_json(comments::changes(was, comments::of(after)))};
}
} // namespace tsugumi::host
