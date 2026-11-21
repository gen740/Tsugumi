#include "canvas_scene.hh"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <ranges>

#include "tsugumi/util/json_object.hh"

namespace tsugumi::linux_ui {
namespace {

// The placeholder box (see canvas_scene.hh).
constexpr double kCardWidth = 220;
constexpr double kCardHeight = 80;

auto Number(std::string_view raw, double fallback) -> double {
  try {
    return raw.empty() || raw == "null" ? fallback
                                        : std::stod(std::string(raw));
  } catch (const std::exception&) {
    return fallback;
  }
}

auto ObjectOrEmpty(std::string_view raw) -> json::Members {
  if (raw.empty() || raw.front() != '{') return {};
  try {
    return json::Object(raw);
  } catch (const std::exception&) {
    return {};
  }
}

auto ArrayOrEmpty(std::string_view raw) -> std::vector<std::string> {
  if (raw.empty() || raw.front() != '[') return {};
  try {
    return json::Array(raw);
  } catch (const std::exception&) {
    return {};
  }
}

}  // namespace

auto Scene::Find(std::string_view id) const -> const SceneCard* {
  const auto found = std::ranges::find(cards, id, &SceneCard::id);
  return found == cards.end() ? nullptr : &*found;
}

auto Scene::CardBox(const SceneCard& card) -> Box {
  return {card.x, card.y, kCardWidth, kCardHeight};
}

auto Scene::WireEnds(const SceneWire& wire) const -> std::pair<Point, Point> {
  const auto* from = Find(wire.from);
  const auto* to = Find(wire.to);
  if (!from || !to) return {};
  const auto a = CardBox(*from);
  const auto b = CardBox(*to);
  if (top_to_bottom)
    return {{a.x + (a.width / 2), a.y + a.height}, {b.x + (b.width / 2), b.y}};
  return {{a.x + a.width, a.y + (a.height / 2)}, {b.x, b.y + (b.height / 2)}};
}

auto Scene::Bounds() const -> Box {
  if (cards.empty()) return {};
  double left = std::numeric_limits<double>::max();
  double top = left;
  double right = std::numeric_limits<double>::lowest();
  double bottom = right;
  for (const auto& card : cards) {
    const auto box = CardBox(card);
    left = std::min(left, box.x);
    top = std::min(top, box.y);
    right = std::max(right, box.x + box.width);
    bottom = std::max(bottom, box.y + box.height);
  }
  return {left, top, right - left, bottom - top};
}

auto Scene::CardAt(double x, double y) const -> const SceneCard* {
  // The last drawn is on top.
  for (const auto& card : std::views::reverse(cards))
    if (CardBox(card).Contains(x, y)) return &card;
  return nullptr;
}

auto StatesKnownType(std::string_view card_json) -> bool {
  static constexpr std::array<std::string_view, 9> kTypes = {
      "code", "draft", "trigger", "input",   "branch",
      "fork", "merge", "collect", "subgraph"};
  const auto card = ObjectOrEmpty(card_json);
  const auto type = json::Text(card, "type");
  if (std::ranges::find(kTypes, type) == kTypes.end()) return false;
  return (type == "subgraph") == card.contains("external");
}

auto CompletionDefinitions(const std::vector<Definition>& definitions)
    -> std::string {
  json::Members out;
  for (const auto& definition : definitions)
    out[definition.id] = json::Quote(definition.card_json);
  return json::Encode(out);
}

auto BuildScene(std::string_view project_json, std::string_view layout_json,
                const std::vector<Definition>& definitions) -> Scene {
  Scene scene;
  const auto layout = ObjectOrEmpty(layout_json);
  // Without "flow" the other UIs infer it from Card places (presentation, not
  // hostd's); this one uses topToBottom, as CompleteLayout does.
  scene.top_to_bottom =
      json::Text(layout, "flow", "topToBottom") != "leftToRight";
  const auto places = ObjectOrEmpty(json::Get(layout, "cards", "{}"));
  std::map<std::string, std::string, std::less<>> names;
  std::map<std::string, const Definition*, std::less<>> by_id;
  for (const auto& definition : definitions) {
    by_id[definition.id] = &definition;
    const auto name = json::Text(ObjectOrEmpty(definition.meta_json), "name");
    if (!name.empty()) names[definition.id] = name;
  }
  const auto canvas =
      ObjectOrEmpty(json::Get(ObjectOrEmpty(project_json), "canvas", "{}"));
  for (const auto& raw : ArrayOrEmpty(json::Get(canvas, "cards", "[]"))) {
    SceneCard card;
    card.id = json::Text(ObjectOrEmpty(raw), "id");
    if (card.id.empty()) continue;
    const auto name = names.find(card.id);
    card.name = name != names.end() ? name->second : card.id;
    const auto place = ObjectOrEmpty(json::Get(places, card.id, "{}"));
    card.x = Number(json::Get(place, "x"), 0);
    card.y = Number(json::Get(place, "y"), 0);
    const auto definition = by_id.find(card.id);
    if (definition == by_id.end()) {
      card.definition_error = true;
    } else {
      card.definition_error = !StatesKnownType(definition->second->card_json);
      card.diagnostic = definition->second->diagnostic;
    }
    scene.cards.push_back(std::move(card));
  }
  for (const auto& raw : ArrayOrEmpty(json::Get(canvas, "connections", "[]"))) {
    const auto connection = ObjectOrEmpty(raw);
    const auto from = ObjectOrEmpty(json::Get(connection, "from", "{}"));
    const auto to = ObjectOrEmpty(json::Get(connection, "to", "{}"));
    SceneWire wire{json::Text(from, "card"), json::Text(from, "port"),
                   json::Text(to, "card"), json::Text(to, "port")};
    // Wires to a Card in Card's bar and the like are not drawn here.
    if (scene.Find(wire.from) && scene.Find(wire.to))
      scene.wires.push_back(std::move(wire));
  }
  return scene;
}

auto ProjectTitle(std::string_view project_json, std::string fallback)
    -> std::string {
  return json::Text(ObjectOrEmpty(project_json), "name", std::move(fallback));
}

}  // namespace tsugumi::linux_ui
