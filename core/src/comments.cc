#include "comments.hh"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

#include "document.hh"
#include "schema.hh"
#include "util/crypto.hh"
#include "versions.hh"

namespace tsugumi::comments {

namespace {

using Object = schema::v::Mapping<schema::v::String, schema::Json>;
constexpr const char* kAgent = "agent";

std::string text_of(const auto& field) {
  const auto* v = schema::get(field);
  return v ? std::string(*v) : std::string();
}

std::optional<std::string> meta_text(const Document& doc, const SourceReader& read, const Node& n) {
  if (n.project_dir.empty()) {
    auto it = doc.definitions.find(n.path.back());
    return it == doc.definitions.end() ? std::nullopt : it->second.meta_json;
  }
  return read(n.def_dir + "_meta.json");
}

// By id, the Card's own first: a copied Card in Card has comments with the same ids.
const Comment* find(const std::vector<Comment>& list, const std::string& card, const std::string& id) {
  const Comment* other = nullptr;
  for (const auto& c : list) {
    if (c.id != id) continue;
    if (c.card == card) return &c;
    if (!other) other = &c;
  }
  return other;
}

// The Cards a save changed, beyond what _meta.json says: Cards that were only
// marked did not change.
std::vector<std::string> changed_cards(const Document& before, const Store::Staged& staged) {
  std::set<std::string> out;
  const auto& d = staged.delta;
  for (const auto& id : d.definitions_upsert) {
    auto was = before.definitions.find(id);
    auto now = staged.doc->definitions.find(id);
    if (was == before.definitions.end() || now == staged.doc->definitions.end() ||
        was->second.card_json != now->second.card_json || was->second.link_json != now->second.link_json)
      out.insert(id);
  }
  for (const auto& id : d.definitions_removed) out.insert(id);
  for (const auto* paths : {&d.sources_upsert, &d.sources_removed})
    for (const auto& path : *paths)
      if (std::filesystem::path(path).filename() != "_meta.json")
        if (auto card = Versions::card_of(path); card && !card->empty()) out.insert(*card);
  return {out.begin(), out.end()};
}

void set_field(auto& comment, const std::string& name, const std::optional<hosq::json::GenericDom<>>& value) {
  std::remove_reference_t<decltype(comment)> next;
  for (const auto& [key, field] : comment)
    if (key != name) next.emplace_back(key, field);
  if (value) next.emplace_back(name, *value);
  comment = std::move(next);
}

std::optional<hosq::json::GenericDom<>> text_value(const std::string& s) {
  if (s.empty()) return std::nullopt;
  return schema::string_value(s);
}

// _meta.json with its comments stamped; the same text when there is nothing to stamp.
std::string stamped(const std::string& text, const std::string& card, const std::vector<Comment>& before,
                    const std::vector<std::string>& changed) {
  Object root;
  if (schema::decode(schema::capture(text), root)) return text;
  auto now = parse(card, text);
  bool any = false;
  for (auto& [name, value] : root.value()) {
    if (name != "comments") continue;
    schema::v::Array<Object> list;
    if (schema::decode(value, list) || list.value().size() != now.size()) return text;
    std::set<std::string> ids;
    for (size_t i = 0; i < now.size(); ++i) {
      auto& c = now[i];
      auto& field = list.value()[i];
      std::string id = c.id;
      if (id.empty() || !ids.insert(id).second) {
        id = crypto::uuid4();
        ids.insert(id);
      }
      const Comment* was = find(before, card, c.id);
      std::string author = was ? was->author : kAgent;
      std::string edited_by = was ? (was->text != c.text ? kAgent : was->edited_by) : std::string();
      bool state_changed = was ? was->resolved != c.resolved : c.resolved;
      std::string state_by = state_changed ? kAgent : was ? was->state_by : std::string();
      auto cards = state_changed ? (c.resolved ? changed : std::vector<std::string>{}) : was ? was->cards : std::vector<std::string>{};
      if (id == c.id && author == c.author && edited_by == c.edited_by && state_by == c.state_by && cards == c.cards) continue;
      any = true;
      if (id != c.id) set_field(field, "id", text_value(id));
      set_field(field, "author", text_value(author));
      set_field(field, "editedBy", text_value(edited_by));
      set_field(field, "stateBy", text_value(state_by));
      if (cards.empty()) {
        set_field(field, "cards", std::nullopt);
      } else {
        schema::v::Array<schema::v::String> names;
        for (const auto& k : cards) names.value().push_back(k);
        set_field(field, "cards", schema::capture(schema::encode(names)));
      }
    }
    if (any) value = schema::capture(schema::encode(list));
  }
  return any ? schema::pretty_text(schema::encode(root)) : text;
}

}  // namespace

std::vector<Comment> parse(const std::string& card, const std::string& meta_json) {
  std::vector<Comment> out;
  schema::CardMeta meta;
  if (schema::decode(meta_json, meta)) return out;
  const auto* comments = schema::get(meta.comments);
  if (!comments) return out;
  for (const auto& c : *comments) {
    Comment o;
    o.card = card;
    o.id = text_of(c.id);
    o.text = text_of(c.text);
    if (const auto* r = schema::get(c.resolved)) o.resolved = *r;
    o.resolution = text_of(c.resolution);
    o.author = text_of(c.author);
    o.edited_by = text_of(c.edited_by);
    o.state_by = text_of(c.state_by);
    if (const auto* cards = schema::get(c.cards))
      for (const auto& k : *cards) o.cards.emplace_back(k);
    out.push_back(std::move(o));
  }
  return out;
}

std::vector<Comment> of(const Document& doc, const SourceReader& read) {
  std::vector<Comment> out;
  if (!doc.resolution) return out;
  for (const auto& n : doc.resolution->nodes) {
    std::optional<std::string> text;
    try {
      text = meta_text(doc, read, n);
    } catch (const DocumentError&) {
    }
    if (!text) continue;
    auto list = parse(path_key(n.path), *text);
    out.insert(out.end(), std::make_move_iterator(list.begin()), std::make_move_iterator(list.end()));
  }
  return out;
}

std::vector<Change> changes(const std::vector<Comment>& before, const std::vector<Comment>& after) {
  std::vector<Change> out;
  for (const auto& c : after) {
    const Comment* was = find(before, c.card, c.id);
    if (!was) {
      if (c.resolved) out.push_back({c.card, c.text, "resolved", c.state_by, c.resolution, c.cards});
      else out.push_back({c.card, c.text, "added", c.author, "", {}});
    } else if (was->resolved != c.resolved) {
      if (c.resolved) out.push_back({c.card, c.text, "resolved", c.state_by, c.resolution, c.cards});
      else out.push_back({c.card, c.text, "reopened", c.state_by, "", {}});
    } else if (was->text != c.text) {
      out.push_back({c.card, c.text, "edited", c.edited_by, c.resolved ? c.resolution : "", {}});
    }
  }
  for (const auto& c : before)
    if (!find(after, c.card, c.id)) out.push_back({c.card, c.text, "removed", "", "", {}});
  std::stable_sort(out.begin(), out.end(), [](const Change& a, const Change& b) { return a.card < b.card; });
  return out;
}

bool stamp_model_save(const Document& before, const SourceReader& read_before, const Store::Staged& staged,
                      const SourceReader& read_after, std::vector<ChangeIn>& changes) {
  if (!staged.doc->resolution) return false;
  auto was = of(before, read_before);
  auto changed = changed_cards(before, staged);
  bool any = false;
  for (const auto& n : staged.doc->resolution->nodes) {
    std::optional<std::string> text;
    try {
      text = meta_text(*staged.doc, read_after, n);
    } catch (const DocumentError&) {
    }
    if (!text) continue;
    auto now = stamped(*text, path_key(n.path), was, changed);
    if (now == *text) continue;
    const bool inner = !n.project_dir.empty();
    auto path = n.def_dir + "_meta.json";
    for (auto& c : changes) {
      if (!inner && c.kind == ChangeIn::Kind::PutDefinition && c.id == n.path.back()) {
        c.meta_json = now;
        any = true;
      } else if (inner && c.path == path && (c.kind == ChangeIn::Kind::PutSource || c.kind == ChangeIn::Kind::PatchSource)) {
        c.kind = ChangeIn::Kind::PutSource;
        c.bytes = now;
        c.patches.clear();
        any = true;
      }
    }
  }
  return any;
}

}  // namespace tsugumi::comments
