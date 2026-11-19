#include "host/comments.hh"

#include <set>

#include "tsugumi/util/json.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::comments {

namespace {

using Object = schema::v::Mapping<schema::v::String, schema::Json>;
constexpr const char* kAgent = "agent";

void set_field(auto& comment, const std::string& name,
               const std::optional<hosq::json::GenericDom<>>& value) {
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

}  // namespace

// _meta.json with its comments stamped; the same text when there is nothing to
// stamp.
std::string stamped(const std::string& text, const std::string& card,
                    const std::vector<Comment>& before,
                    const std::vector<std::string>& changed,
                    const ReserveComment& reserve) {
  Object root;
  if (schema::decode(schema::capture(text), root)) return text;
  auto now = parse(card, text);
  bool any = false;
  for (auto& [name, value] : root.value()) {
    if (name != "comments") continue;
    schema::v::Array<Object> list;
    if (schema::decode(value, list) || list.value().size() != now.size())
      return text;
    std::set<std::string> ids, reserved;
    for (const auto& c : now) reserved.insert(c.id);
    uint64_t next_id = 0;
    for (size_t i = 0; i < now.size(); ++i) {
      auto& c = now[i];
      auto& field = list.value()[i];
      std::string id = c.id;
      if (id.empty() || !ids.insert(id).second) {
        do {
          id = reserve ? reserve(card, {reserved.begin(), reserved.end()})
                       : std::to_string(++next_id);
        } while (reserved.contains(id));
        reserved.insert(id);
        ids.insert(id);
      }
      const Comment* was = id == c.id ? find(before, card, c.id) : nullptr;
      std::string author = was ? was->author : kAgent;
      std::string edited_by =
          was ? (was->text != c.text ? kAgent : was->edited_by) : std::string();
      bool state_changed = was ? was->resolved != c.resolved : c.resolved;
      std::string state_by = state_changed ? kAgent
                             : was         ? was->state_by
                                           : std::string();
      auto cards = state_changed
                       ? (c.resolved ? changed : std::vector<std::string>{})
                   : was ? was->cards
                         : std::vector<std::string>{};
      if (id == c.id && author == c.author && edited_by == c.edited_by &&
          state_by == c.state_by && cards == c.cards)
        continue;
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

auto record_json(const std::vector<Change>& changes) -> std::string {
  std::vector<std::string> out;
  for (const auto& c : changes) {
    json::Members value{{"card", json::Quote(c.card)},
                        {"text", json::Quote(c.text)},
                        {"change", json::Quote(c.change)}};
    if (!c.by.empty()) value["by"] = json::Quote(c.by);
    if (!c.resolution.empty()) value["resolution"] = json::Quote(c.resolution);
    std::vector<std::string> cards;
    for (const auto& id : c.cards) cards.push_back(json::Quote(id));
    value["cards"] = json::Encode(cards);
    out.push_back(json::Encode(value));
  }
  return json::Encode(out);
}

}  // namespace tsugumi::comments
