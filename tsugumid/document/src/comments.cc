#include "tsugumi/document/comments.hh"

#include <algorithm>

#include "tsugumi/util/json_object.hh"

namespace tsugumi::comments {

// By id, the Card's own first: a copied Card in Card has comments with the same
// ids.
auto find(const std::vector<Comment>& list, const std::string& card,
          const std::string& id) -> const Comment* {
  const Comment* other = nullptr;
  for (const auto& c : list) {
    if (c.id != id) continue;
    if (c.card == card) return &c;
    if (!other) other = &c;
  }
  return other;
}

std::vector<Comment> parse(const std::string& card,
                           const std::string& meta_json) {
  std::vector<Comment> out;
  try {
    auto root = json::Object(meta_json);
    for (const auto& raw : json::Array(json::Get(root, "comments", "[]"))) {
      const auto c = json::Object(raw);
      Comment o;
      o.card = card;
      o.id = json::Text(c, "id");
      o.text = json::Text(c, "text");
      o.resolved = json::Bool(json::Get(c, "resolved"));
      o.resolution = json::Text(c, "resolution");
      o.author = json::Text(c, "author");
      o.edited_by = json::Text(c, "editedBy");
      o.state_by = json::Text(c, "stateBy");
      for (const auto& k : json::Array(json::Get(c, "cards", "[]")))
        o.cards.push_back(json::String(k));
      out.push_back(std::move(o));
    }
  } catch (const std::exception&) {
    return {};
  }
  return out;
}

auto of(const Files& files) -> std::vector<Comment> {
  std::vector<Comment> out;
  for (const auto& [path, text] : files) {
    if (!is_meta(path)) continue;
    if (auto card = card_of(path, files)) {
      auto list = parse(*card, text);
      out.insert(out.end(), list.begin(), list.end());
    }
  }
  return out;
}

std::vector<Change> changes(const std::vector<Comment>& before,
                            const std::vector<Comment>& after) {
  std::vector<Change> out;
  for (const auto& c : after) {
    const Comment* was = find(before, c.card, c.id);
    if (!was) {
      if (c.resolved)
        out.push_back(
            {c.card, c.text, "resolved", c.state_by, c.resolution, c.cards});
      else
        out.push_back({c.card, c.text, "added", c.author, "", {}});
    } else if (was->resolved != c.resolved) {
      if (c.resolved)
        out.push_back(
            {c.card, c.text, "resolved", c.state_by, c.resolution, c.cards});
      else
        out.push_back({c.card, c.text, "reopened", c.state_by, "", {}});
    } else if (was->text != c.text) {
      out.push_back({c.card,
                     c.text,
                     "edited",
                     c.edited_by,
                     c.resolved ? c.resolution : "",
                     {}});
    }
  }
  for (const auto& c : before)
    if (!find(after, c.card, c.id))
      out.push_back({c.card, c.text, "removed", "", "", {}});
  std::stable_sort(
      out.begin(), out.end(),
      [](const Change& a, const Change& b) { return a.card < b.card; });
  return out;
}

auto is_meta(const std::string& path) -> bool {
  auto rel = path;
  while (rel.starts_with("externals/")) {
    const auto slash = rel.find('/', 10);
    if (slash == std::string::npos) return false;
    rel.erase(0, slash + 1);
  }
  if (!rel.starts_with("definitions/")) return false;
  const auto slash = rel.find('/', 12);
  return slash != std::string::npos && rel.substr(slash + 1) == "_meta.json";
}

auto card_of(const std::string& path, const Files& files)
    -> std::optional<std::string> {
  std::string rest = path, prefix, directory;
  while (rest.starts_with("externals/")) {
    auto slash = rest.find('/', 10);
    if (slash == std::string::npos) return std::nullopt;
    const auto external = rest.substr(10, slash - 10);
    auto id = external;
    for (const auto& [link, text] : files) {
      if (!link.starts_with(directory + "definitions/") ||
          !link.ends_with("/_card.json"))
        continue;
      auto local = link.substr(directory.size());
      if (local.find('/', 12) != local.size() - 11) continue;
      try {
        if (::tsugumi::json::Text(::tsugumi::json::Object(text), "external") ==
            external) {
          id = local.substr(12, local.size() - 23);
          break;
        }
      } catch (const std::exception&) {
      }
    }
    prefix += id + "/";
    directory += "externals/" + external + "/";
    rest.erase(0, slash + 1);
  }
  if (!rest.starts_with("definitions/")) return std::nullopt;
  const auto slash = rest.find('/', 12);
  if (slash == std::string::npos) return std::nullopt;
  return prefix + rest.substr(12, slash - 12);
}

}  // namespace tsugumi::comments
