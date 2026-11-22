#include "host/comments.hh"

#include <set>

#include "tsugumi/document/text.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::comments {

namespace {
constexpr const char* kAgent = "agent";
}  // namespace

// _comments.json with its comments stamped; the same text when there is
// nothing to stamp or it cannot be read (the core then rejects it).
std::string stamped(const std::string& text, const std::string& card,
                    const std::vector<Comment>& before,
                    const std::vector<std::string>& changed,
                    const ReserveComment& reserve) {
  namespace D = Tsugumi::Document;
  using document_text::Schema;
  D::CommentsT root;
  try {
    document_text::root<D::Comments>(
        document_text::parse(Schema::Comments, "_comments.json", text))
        .UnPackTo(&root);
  } catch (const std::exception&) {
    return text;
  }
  const auto now = parse(card, text);
  if (now.size() != root.comments.size()) return text;
  std::set<std::string> ids, reserved;
  for (const auto& c : now) reserved.insert(c.id);
  uint64_t next_id = 0;
  bool any = false;
  for (size_t i = 0; i < now.size(); ++i) {
    const auto& c = now[i];
    auto& field = *root.comments[i];
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
    field.id = id;
    field.author = author;
    field.edited_by = edited_by;
    field.state_by = state_by;
    field.cards = cards;
  }
  return any ? document_text::print(Schema::Comments,
                                    document_text::pack<D::Comments>(root))
             : text;
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
