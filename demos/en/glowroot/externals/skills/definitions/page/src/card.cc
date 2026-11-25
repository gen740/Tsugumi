#include "page.hh"

#include <charconv>

// Shows the skill tree that skills/validate priced. A click on a node leaves
// with that purchase for validate; Back leaves the screen.
namespace {

const auto& P = Card::params;

constexpr uint32_t kBranchColors[] = {0xffd77aff, 0x9fe08aff, 0x8fd3ffff, 0xffa8e0ff};

auto node_place(size_t node, size_t count) -> std::pair<float, float> {
  if (node + 1 == count) return {640, 120};  // the Bloom, above the three branches
  const size_t branch = node / 4;
  const size_t tier = node % 4;
  return {320 + 320 * static_cast<float>(branch), 590 - 105 * static_cast<float>(tier)};
}

}  // namespace

auto run(const Card::Inputs& inputs) -> Card::Output {
  const auto* offer = inputs.offer;
  auto turn = game::own_turn(offer->turn());
  game::Page page(game::session_of(*turn.view));
  if (page.closed()) return {};
  game::Stopwatch clock;
  Card::Output output;

  game::Scene scene("skills");
  scene.label(P.title, 640, 48, 40, game::kText, 1);
  scene.counter(P.seeds_text, turn.state->seeds, 60, 64, 28, game::kGreen, 0);
  for (size_t branch = 0; branch < 3; ++branch)
    scene.label(P.branches.at(branch), 320 + 320 * static_cast<float>(branch), 660, 24,
                kBranchColors[branch], 1);
  const size_t count = offer->nodes() ? offer->nodes()->size() : 0;
  for (size_t node = 0; node < count; ++node) {
    const auto* item = offer->nodes()->Get(static_cast<flatbuffers::uoffset_t>(node));
    const auto [x, y] = node_place(node, count);
    std::string detail = item->effect() ? item->effect()->str() : "";
    if (item->state() != 3)
      detail += "  ·  " + game::replace(P.cost_text, "{}", std::to_string(item->cost()));
    if (item->note() && item->note()->size()) detail += "  ·  " + item->note()->str();
    const uint32_t color = node + 1 == count ? kBranchColors[3] : kBranchColors[node / 4];
    scene.node("node" + std::to_string(node), item->name() ? item->name()->str() : "", detail, x,
               y, item->state(), item->parent(), color);
  }
  if (offer->note()) scene.label(offer->note()->str(), 640, 700, 20, game::kDim, 1);
  if (offer->bought() >= 0 && static_cast<size_t>(offer->bought()) < count) {
    const auto [x, y] = node_place(static_cast<size_t>(offer->bought()), count);
    scene.fx(1, x, y);
  }
  scene.button("back", P.back_text, "", 1040, 30, 200, 60, true, 0);
  page.show(scene);

  // Once the Bloom is bought, the grow screen shows it.
  double shown = 0;
  while (true) {
    const auto event = page.next();
    const double ms = clock.lap();
    shown += ms;
    turn.state->elapsed_ms += static_cast<uint64_t>(ms);
    if (event.kind == game::Event::kClosed || event.kind == game::Event::kCancelled)
      return output;
    if (turn.state->bloom && shown >= P.bloom_pause_ms) break;
    if (event.kind != game::Event::kButton) continue;
    if (event.button == "back") break;
    if (event.button.starts_with("node")) {
      size_t node = 0;
      const std::string_view index = std::string_view(event.button).substr(4);
      const auto end = index.data() + index.size();
      const auto [parsed, error] = std::from_chars(index.data(), end, node);
      if (error != std::errc{} || parsed != end) continue;
      if (node >= count) continue;
      page.leave();
      auto& purchase = output.buy.emplace();
      purchase.node = static_cast<uint8_t>(node);
      purchase.turn = std::make_unique<glow::TurnT>(std::move(turn));
      return output;
    }
  }
  page.leave();
  output.back = std::move(turn);
  return output;
}
