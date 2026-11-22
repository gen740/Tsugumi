#include "tsugumi/document/comments.hh"

#include <algorithm>

#include "tsugumi/document/text.hh"

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
                           const std::string& comments_json) {
  std::vector<Comment> out;
  std::string buffer;
  try {
    buffer = document_text::parse(document_text::Schema::Comments,
                                  "_comments.json", comments_json);
  } catch (const std::exception&) {
    return {};
  }
  using document_text::str;
  const auto& root =
      document_text::root<Tsugumi::Document::Comments>(buffer);
  for (const auto* c : *root.comments()) {
    Comment o;
    o.card = card;
    o.id = str(c->id());
    o.text = c->text()->str();
    o.resolved = c->resolved().value_or(false);
    o.resolution = str(c->resolution());
    o.author = str(c->author());
    o.edited_by = str(c->edited_by());
    o.state_by = str(c->state_by());
    if (const auto* cards = c->cards())
      for (const auto* k : *cards) o.cards.push_back(k->str());
    out.push_back(std::move(o));
  }
  return out;
}

auto of(const Files& files) -> std::vector<Comment> {
  std::vector<Comment> out;
  for (const auto& [path, text] : files) {
    if (!is_comments(path)) continue;
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

auto is_comments(const std::string& path) -> bool {
  auto rel = path;
  while (rel.starts_with("externals/")) {
    const auto slash = rel.find('/', 10);
    if (slash == std::string::npos) return false;
    rel.erase(0, slash + 1);
  }
  if (!rel.starts_with("definitions/")) return false;
  const auto slash = rel.find('/', 12);
  return slash != std::string::npos &&
         rel.substr(slash + 1) == "_comments.json";
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
        const auto buffer = document_text::parse(document_text::Schema::Card,
                                                 link, text);
        if (document_text::str(
                document_text::root<Tsugumi::Document::Card>(buffer)
                    .external()) == external) {
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
