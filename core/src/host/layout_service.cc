#include "host/layout_service.hh"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "agent/wire.hh"
#include "host/layout.hh"
#include "host/layout_schema.hh"

namespace tsugumi::host {
namespace R = Tsugumi::Host;
namespace {
[[noreturn]] auto invalid_layout(const std::string& message) -> void {
  throw LayoutFailure(message);
}
auto decode_layout(const std::string& text) -> layout_schema::Layout {
  if (text.size() > 8 * 1024 * 1024)
    invalid_layout("layout.json exceeds 8 MiB");
  layout_schema::Layout out;
  if (auto error = schema::decode(text, out))
    invalid_layout("layout.json: " + error->message);
  return out;
}
// Reject non-finite/unbounded geometry before sorting or converting to integer
// grid constraints. The transport already validates all required
// vectors/tables.
auto measured(double v) -> double {
  if (!std::isfinite(v) || std::abs(v) > 1e7)
    throw LayoutFailure("layout geometry is out of range");
  return v;
}
auto size(double v) -> double {
  if (measured(v) < 0) throw LayoutFailure("layout size must be nonnegative");
  return v;
}
auto scene_of(const R::LayoutScene& in) -> layout::Scene {
  layout::Scene s;
  const auto flow = in.flow()->str();
  if (flow != "topToBottom" && flow != "leftToRight")
    throw LayoutFailure("unknown layout flow");
  s.top_to_bottom = flow == "topToBottom";
  s.grid = in.grid() == 0 ? 20 : measured(in.grid());
  if (s.grid < 1) throw LayoutFailure("layout grid must be at least 1");
  s.snap = in.snap();
  auto ports = [](const auto* ps) {
    std::vector<layout::Port> out;
    std::set<std::string> names;
    for (const auto* p : *ps) {
      if (!names.insert(p->name()->str()).second)
        throw LayoutFailure("duplicate layout port");
      out.push_back({p->name()->str(), measured(p->x()), measured(p->y())});
    }
    return out;
  };
  std::set<std::string> ids;
  for (const auto* c : *in.cards()) {
    const auto id = c->id()->str();
    if (id.empty() || id.find('\0') != std::string::npos ||
        !ids.insert(id).second)
      throw LayoutFailure("invalid or duplicate layout Card id");
    s.cards.push_back({id, measured(c->x()), measured(c->y()), size(c->width()),
                       size(c->height()), ports(c->inputs()),
                       ports(c->outputs()), c->reorderable()});
  }
  for (const auto* w : *in.wires())
    s.wires.push_back({w->from()->str(), w->from_port()->str(), w->to()->str(),
                       w->to_port()->str()});
  auto boxes = [](const auto* bs) {
    std::vector<layout::Box> out;
    for (const auto* b : *bs)
      out.push_back({b->id()->str(), measured(b->x()), measured(b->y()),
                     size(b->width()), size(b->height())});
    return out;
  };
  s.frames = boxes(in.frames());
  s.notes = boxes(in.notes());
  auto bar = [](const R::LayoutBar* b) {
    layout::Bar out;
    if (!b) return out;
    for (const auto* e : *b->ends())
      out.ends.push_back(
          {e->name()->str(), e->card()->str(), e->port()->str()});
    if (b->x().has_value() != b->y().has_value())
      throw LayoutFailure("layout bar needs both coordinates");
    if (b->x()) out.at = layout::Point{measured(*b->x()), measured(*b->y())};
    return out;
  };
  s.inputs = bar(in.inputs());
  s.outputs = bar(in.outputs());
  return s;
}
}  // namespace

auto parse_layout_json(const std::string& text) -> layout_schema::Layout {
  auto l = decode_layout(text);
  if (const auto* cards = schema::get(l.cards))
    for (const auto& [key, pos] : cards->entries())
      if (!pos.x->has_value() || !pos.y->has_value())
        invalid_layout("layout.json: card " + key + " needs x and y");
  if (const auto* notes = schema::get(l.notes))
    for (const auto& n : *notes)
      if (!n.id->has_value() || !n.text->has_value() || !n.x->has_value() ||
          !n.y->has_value())
        invalid_layout("layout.json: notes need id, text, x and y");
  if (const auto* displays = schema::get(l.displays))
    for (const auto& [key, d] : displays->entries())
      if (!d.x->has_value() || !d.y->has_value() || !d.width->has_value() ||
          !d.height->has_value())
        invalid_layout("layout.json: display " + key +
                       " needs x, y, width and height");
  return l;
}

auto layout_without_cards(const std::string& text,
                          const std::set<std::string>& gone) -> std::string {
  using Object = schema::v::Mapping<schema::v::String, schema::Json>;
  using hosq::json::GenericDom;
  auto as = []<class S>(const GenericDom<>& value, S& out) {
    if (auto err = schema::decode(value, out))
      invalid_layout("layout.json: " + err->message);
  };
  bool changed = false;
  // Where a key or id of a Card goes; nothing when its Card is gone.
  auto follow = [&](const std::string& key) -> std::optional<std::string> {
    auto card = key.substr(0, key.find('/'));
    if (gone.count(card)) {
      changed = true;
      return std::nullopt;
    }
    return key;
  };
  auto cards_by_key = [&](const GenericDom<>& value) {
    Object in, out;
    as(value, in);
    for (const auto& [key, entry] : in.value())
      if (follow(key)) out.value().emplace_back(key, entry);
    return schema::capture(schema::encode(out));
  };
  auto group_ids = [&](const GenericDom<>& value) {
    schema::v::Array<schema::v::String> in, out;
    as(value, in);
    for (const auto& id : in.value())
      if (auto to = follow(id)) out.value().push_back(*to);
    return schema::capture(schema::encode(out));
  };
  auto group_outputs = [&](const GenericDom<>& value) {
    schema::v::Array<Object> in, out;
    as(value, in);
    for (auto port : in.value()) {
      bool kept = true;
      for (auto& [name, field] : port) {
        if (name != "card") continue;
        schema::v::String id;
        as(field, id);
        if (auto to = follow(id.value()))
          field = schema::string_value(*to);
        else
          kept = false;
      }
      if (kept) out.value().push_back(std::move(port));
    }
    return schema::capture(schema::encode(out));
  };

  Object root;
  as(schema::capture(text), root);
  for (auto& [field, value] : root.value()) {
    if (field == "cards" || field == "displays" || field == "boundaries") {
      value = cards_by_key(value);
    } else if (field == "groups") {
      schema::v::Array<Object> groups;
      as(value, groups);
      for (auto& group : groups.value())
        for (auto& [name, member] : group) {
          if (name == "cards")
            member = group_ids(member);
          else if (name == "outputs")
            member = group_outputs(member);
        }
      value = schema::capture(schema::encode(groups));
    }
  }
  return changed ? schema::pretty_text(schema::encode(root)) : text;
}

auto ValidateLayout(const std::string& text,
                    const std::set<std::string>& removed) -> std::string {
  parse_layout_json(text);
  return layout_without_cards(text, removed);
}

auto VersionCanvas(const R::VersionCanvasRequest& in) -> R::VersionCanvasT {
  struct Card {
    std::string id;
    double x = 0, y = 0;
    std::string kind;
    uint32_t fan = 0;
    bool collapsed = false;
  };
  struct Canvas {
    std::vector<Card> cards;
    std::vector<std::pair<std::string, std::string>> wires;
    std::string flow = "leftToRight";
  } out;
  std::map<std::string, size_t> index;
  for (const auto* card : *in.cards()) {
    index[card->id()->str()] = out.cards.size();
    out.cards.push_back(
        {card->id()->str(), 0, 0, card->kind()->str(), card->fan(), false});
  }
  for (const auto* wire : *in.wires())
    if (index.contains(wire->from()->str()) &&
        index.contains(wire->to()->str()))
      out.wires.emplace_back(wire->from()->str(), wire->to()->str());
  auto place = [&]() -> void {
    // Saved positions, when every Card has one.
    std::map<std::string, layout_schema::Position> saved;
    layout_schema::Layout layout;
    auto layout_text = in.layout_json()->str();
    if (!schema::decode(layout_text, layout))
      if (const auto* positions = schema::get(layout.cards))
        for (const auto& [key, position] : positions->entries())
          if (schema::get(position.x) && schema::get(position.y))
            saved[key] = position;
    bool placed = !out.cards.empty();
    for (const auto& card : out.cards) placed = placed && saved.count(card.id);
    if (placed) {
      for (auto& card : out.cards) {
        const auto& position = saved[card.id];
        card.x = *schema::get(position.x);
        card.y = *schema::get(position.y);
        const auto* collapsed = schema::get(position.collapsed);
        card.collapsed = collapsed && *collapsed;
      }
      // The saved direction; without one, read from where the wired Cards sit,
      // as the clients do for the canvas.
      const auto* flow = schema::get(layout.flow);
      if (flow && (*flow == "topToBottom" || *flow == "leftToRight")) {
        out.flow = *flow;
      } else {
        double across = 0;
        double down = 0;
        for (const auto& [a, b] : out.wires) {
          across += std::abs(out.cards[index[b]].x - out.cards[index[a]].x);
          down += std::abs(out.cards[index[b]].y - out.cards[index[a]].y);
        }
        if (down > 0 && down >= across) out.flow = "topToBottom";
      }
      return;
    }
    // Columns by depth (a wire back in a loop does not push a Card further).
    std::vector<size_t> depth(out.cards.size(), 0);
    for (size_t round = 0; round < out.cards.size(); ++round)
      for (const auto& [a, b] : out.wires)
        depth[index[b]] =
            std::max(depth[index[b]],
                     std::min(depth[index[a]] + 1, out.cards.size() - 1));
    std::map<size_t, int> rows;
    for (size_t i = 0; i < out.cards.size(); ++i) {
      out.cards[i].x = static_cast<double>(depth[i]) * 300;
      out.cards[i].y = rows[depth[i]]++ * 120.0;
    }
  };
  place();
  R::VersionCanvasT result;
  result.flow = out.flow;
  for (const auto& c : out.cards) {
    auto card = std::make_unique<R::VersionCanvasCardT>();
    card->id = c.id;
    card->x = c.x;
    card->y = c.y;
    card->kind = c.kind;
    card->fan = c.fan;
    card->collapsed = c.collapsed;
    result.cards.push_back(std::move(card));
  }
  for (const auto& [a, b] : out.wires) {
    auto wire = std::make_unique<R::VersionCanvasWireT>();
    wire->from = a;
    wire->to = b;
    result.wires.push_back(std::move(wire));
  }
  return result;
}

auto LayoutHandles(R::RequestPayload type) -> bool {
  return type == R::RequestPayload::LayoutArrangeRequest ||
         type == R::RequestPayload::LayoutUntangleRequest ||
         type == R::RequestPayload::LayoutValidateRequest ||
         type == R::RequestPayload::VersionCanvasRequest;
}
auto LayoutHandle(const R::Request& req, uint64_t id) -> std::string {
  if (const auto* v = req.value_as_LayoutValidateRequest()) {
    std::set<std::string> removed;
    if (v->removed_cards())
      for (const auto* card : *v->removed_cards()) removed.insert(card->str());
    R::LayoutValidatedT result;
    result.json = ValidateLayout(v->json()->str(), removed);
    return agent::wire::HostReply(id, std::move(result));
  }
  if (const auto* v = req.value_as_VersionCanvasRequest())
    return agent::wire::HostReply(id, VersionCanvas(*v));
  const auto* a = req.value_as_LayoutArrangeRequest();
  const auto* u = req.value_as_LayoutUntangleRequest();
  const auto scene = scene_of(*(a ? a->scene() : u->scene()));
  std::vector<std::string> around;
  if (u && u->around())
    for (const auto* card : *u->around()) around.push_back(card->str());
  const auto proposal =
      a ? layout::arrange(scene) : layout::untangle(scene, around);
  R::LayoutProposalT result;
  auto places = [](const auto& source, auto& destination) {
    for (const auto& p : source) {
      auto v = std::make_unique<R::LayoutPlaceT>();
      v->id = p.id;
      v->x = p.x;
      v->y = p.y;
      destination.push_back(std::move(v));
    }
  };
  places(proposal.cards, result.cards);
  places(proposal.frames, result.frames);
  places(proposal.notes, result.notes);
  for (const auto& order : proposal.port_orders) {
    auto v = std::make_unique<R::LayoutPortOrderT>();
    v->id = order.id;
    v->inputs = order.inputs;
    v->outputs = order.outputs;
    result.port_orders.push_back(std::move(v));
  }
  return agent::wire::HostReply(id, std::move(result));
}
}  // namespace tsugumi::host
