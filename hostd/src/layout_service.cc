#include "host/layout_service.hh"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "agent/wire.hh"
#include "host/layout.hh"
#include "host/layout_saving.hh"
#include "host/layout_schema.hh"
#include "tsugumi/util/json_object.hh"

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
auto measured(double v, const std::string& field) -> double {
  if (!std::isfinite(v) || std::abs(v) > 1e7)
    throw LayoutFailure(field +
                        ": must be finite and within [-10000000, 10000000]");
  return v;
}
auto size(double v, const std::string& field) -> double {
  if (measured(v, field) < 0)
    throw LayoutFailure(field + ": must be nonnegative");
  return v;
}
auto scene_of(const R::LayoutScene& in) -> layout::Scene {
  layout::Scene s;
  const auto flow = in.flow()->str();
  if (flow != "topToBottom" && flow != "leftToRight")
    throw LayoutFailure("unknown layout flow");
  s.top_to_bottom = flow == "topToBottom";
  s.grid = in.grid() == 0 ? 20 : measured(in.grid(), "scene.grid");
  if (s.grid < 1) throw LayoutFailure("layout grid must be at least 1");
  s.snap = in.snap();
  auto ports = [](const auto* ps, const std::string& field) {
    std::vector<layout::Port> out;
    std::set<std::string> names;
    if (ps)
      for (const auto* p : *ps) {
        if (!names.insert(p->name()->str()).second)
          throw LayoutFailure("duplicate layout port");
        const auto at = field + "[" + std::to_string(out.size()) + "]";
        out.push_back({p->name()->str(), measured(p->x(), at + ".x"),
                       measured(p->y(), at + ".y")});
      }
    return out;
  };
  std::set<std::string> ids;
  if (in.cards())
    for (const auto* c : *in.cards()) {
      const auto id = c->id()->str();
      if (id.empty() || id.find('\0') != std::string::npos ||
          !ids.insert(id).second)
        throw LayoutFailure("invalid or duplicate layout Card id");
      const auto at = "scene.cards[" + std::to_string(s.cards.size()) + "]";
      s.cards.push_back(
          {id, measured(c->x(), at + ".x"), measured(c->y(), at + ".y"),
           size(c->width(), at + ".width"), size(c->height(), at + ".height"),
           ports(c->inputs(), at + ".inputs"),
           ports(c->outputs(), at + ".outputs"), c->reorderable()});
    }
  if (in.wires())
    for (const auto* w : *in.wires())
      s.wires.push_back({w->from()->str(), w->from_port()->str(),
                         w->to()->str(), w->to_port()->str()});
  auto boxes = [](const auto* bs, const std::string& field) {
    std::vector<layout::Box> out;
    if (bs)
      for (const auto* b : *bs) {
        const auto at = field + "[" + std::to_string(out.size()) + "]";
        out.push_back({b->id()->str(), measured(b->x(), at + ".x"),
                       measured(b->y(), at + ".y"),
                       size(b->width(), at + ".width"),
                       size(b->height(), at + ".height")});
      }
    return out;
  };
  s.notes = boxes(in.notes(), "scene.notes");
  auto bar = [](const R::LayoutBar* b, const std::string& field) {
    layout::Bar out;
    if (!b) return out;
    if (b->ends())
      for (const auto* e : *b->ends())
        out.ends.push_back(
            {e->name()->str(), e->card()->str(), e->port()->str()});
    if (b->x().has_value() != b->y().has_value())
      throw LayoutFailure("layout bar needs both coordinates");
    if (b->x())
      out.at = layout::Point{measured(*b->x(), field + ".x"),
                             measured(*b->y(), field + ".y")};
    return out;
  };
  s.inputs = bar(in.inputs(), "scene.inputs");
  s.outputs = bar(in.outputs(), "scene.outputs");
  return s;
}
}  // namespace

auto MeasuredScene(const R::LayoutSceneT& scene) -> layout::Scene {
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(R::LayoutScene::Pack(builder, &scene));
  return scene_of(
      *flatbuffers::GetRoot<R::LayoutScene>(builder.GetBufferPointer()));
}

auto parse_layout_json(const std::string& text) -> layout_schema::Layout {
  auto l = decode_layout(text);
  if (const auto* cards = schema::get(l.cards))
    for (const auto& [key, pos] : cards->entries())
      if (pos.x->has_value() != pos.y->has_value())
        invalid_layout("layout.json: card " + key + " needs both x and y");
  if (const auto* notes = schema::get(l.notes))
    for (const auto& n : *notes)
      if (!n.id->has_value() || !n.text->has_value() || !n.x->has_value() ||
          !n.y->has_value())
        invalid_layout("layout.json: notes need id, text, x and y");
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
    if (field == "cards" || field == "boundaries") {
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
  R::VersionCanvasT result;
  layout_schema::Layout saved;
  if (schema::decode(in.layout_json()->str(), saved)) saved = {};
  std::map<std::string, const layout_schema::Position*> positions;
  if (const auto* entries = schema::get(saved.cards))
    for (const auto& [id, position] : entries->entries())
      positions.emplace(id, &position);
  layout::Scene scene;
  std::vector<std::string> missing;
  std::map<std::string, size_t> index;
  std::set<std::string> placed;
  if (in.cards())
    for (const auto* entry : *in.cards()) {
      auto card = std::make_unique<R::VersionCanvasCardT>();
      card->id = entry->id()->str();
      card->kind = entry->kind()->str();
      card->fan = entry->fan();
      const auto at = positions.find(card->id);
      const auto* position = at != positions.end() ? at->second : nullptr;
      const auto* x = position ? schema::get(position->x) : nullptr;
      const auto* y = position ? schema::get(position->y) : nullptr;
      const auto* collapsed = position ? schema::get(position->collapsed)
                                       : nullptr;
      card->collapsed = collapsed && *collapsed;
      if (x && y) {
        card->x = *x;
        card->y = *y;
        placed.insert(card->id);
      } else {
        missing.push_back(card->id);
      }
      index[card->id] = result.cards.size();
      result.cards.push_back(std::move(card));
    }
  double across = 0, down = 0;
  if (in.wires())
    for (const auto* entry : *in.wires()) {
      const auto from = entry->from()->str(), to = entry->to()->str();
      if (!index.contains(from) || !index.contains(to)) continue;
      auto wire = std::make_unique<R::VersionCanvasWireT>();
      wire->from = from;
      wire->to = to;
      result.wires.push_back(std::move(wire));
      scene.wires.push_back({from, {}, to, {}});
      if (placed.contains(from) && placed.contains(to)) {
        const auto& a = *result.cards[index.at(from)];
        const auto& b = *result.cards[index.at(to)];
        across += std::abs(b.x - a.x);
        down += std::abs(b.y - a.y);
      }
    }
  const auto* flow = schema::get(saved.flow);
  scene.top_to_bottom = flow && (*flow == "topToBottom" || *flow == "leftToRight")
                            ? *flow == "topToBottom"
                            : down > 0 && down >= across;
  result.flow = scene.top_to_bottom ? "topToBottom" : "leftToRight";
  if (!missing.empty()) {
    // Reserve room for the preview's Card and any ports shown on its outline.
    for (const auto& card : result.cards) {
      const double port_span = 64.0 * card->fan + 24;
      scene.cards.push_back(
          {card->id, card->x, card->y,
           scene.top_to_bottom ? std::max(320.0, port_span) : 320.0,
           scene.top_to_bottom ? 120.0 : std::max(120.0, port_span),
           {}, {}, false});
    }
    for (const auto& p : layout::place_missing(scene, missing).cards) {
      auto& card = *result.cards[index.at(p.id)];
      card.x = p.x;
      card.y = p.y;
    }
  }
  return result;
}

auto LayoutHandles(R::RequestPayload type) -> bool {
  return type == R::RequestPayload::LayoutValidateRequest ||
         type == R::RequestPayload::LayoutCompleteRequest ||
         type == R::RequestPayload::VersionCanvasRequest;
}
auto LayoutHandle(const R::Request& req, uint64_t id) -> std::string {
  if (const auto* v = req.value_as_LayoutCompleteRequest()) {
    const auto definitions =
        ::tsugumi::json::Object(v->definitions_json()->str());
    R::LayoutValidatedT result;
    result.json = CompleteLayout(
        v->project_json()->str(), v->layout_json()->str(),
        [&](const std::string& path) -> std::optional<LayoutFile> {
          const auto id = path.substr(12, path.size() - 23);
          if (!definitions.contains(id)) return std::nullopt;
          return LayoutFile{::tsugumi::json::Text(definitions, id), {}};
        });
    return agent::wire::HostReply(id, std::move(result));
  }
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
  throw LayoutFailure("Unknown layout query");
}
}  // namespace tsugumi::host
