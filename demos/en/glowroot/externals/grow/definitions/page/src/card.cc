#include "page.hh"

#include <charconv>

namespace {

const auto& P = Card::params;

auto root_cost(const glow::GameStateT& state, size_t kind) -> double {
  return P.root_costs.at(kind) * std::pow(P.cost_growth, state.roots.at(kind));
}

auto income(const glow::GameStateT& state) -> double {
  double sum = 0;
  for (size_t kind = 0; kind < state.roots.size(); ++kind)
    sum += state.roots[kind] * P.root_outputs.at(kind);
  return sum * state.boosts->roots;
}

auto click_power(const glow::GameStateT& state) -> double {
  return P.click * state.boosts->click;
}

// A kind of root shows once the game has grown a share of its first price.
auto known(const glow::GameStateT& state, size_t kind) -> bool {
  return kind == 0 || state.roots[kind] > 0 || state.roots[kind - 1] > 0 ||
         state.total >= P.root_costs.at(kind) * P.reveal_share;
}

auto plant_size(const glow::GameStateT& state) -> float {
  double owned = 0;
  for (auto count : state.roots) owned += count;
  return static_cast<float>(std::min(1.0, 0.18 + std::log1p(owned) / std::log1p(120.0)));
}

auto scene_of(const glow::GameStateT& state) -> game::Scene {
  game::Scene scene("grow");
  scene.value.plant = plant_size(state);
  scene.button("sprout", "", "", 150, 150, 340, 420, true, 3);
  scene.counter(P.light_text, state.light, 320, 92, 52, game::kGold, 1);
  scene.label(game::replace(P.rate_text, "{}", game::number(income(state))), 320, 136, 22,
              game::kDim, 1);
  scene.label(game::replace(P.click_text, "{}", game::number(click_power(state))), 320, 620,
              20, game::kDim, 1);

  scene.label(P.shop_title, 680, 92, 30, game::kText, 0);
  for (size_t kind = 0; kind < state.roots.size(); ++kind) {
    const float y = 130 + 96 * static_cast<float>(kind);
    if (!known(state, kind)) {
      scene.button("root" + std::to_string(kind), P.unknown_root, "", 680, y, 520, 80, false, 2);
      continue;
    }
    const double cost = root_cost(state, kind);
    const auto detail = game::replace(
        game::replace(P.root_detail, "{cost}", game::number(cost)), "{each}",
        game::number(P.root_outputs.at(kind) * state.boosts->roots));
    scene.button("root" + std::to_string(kind),
                 P.root_names.at(kind) + "  ×" + std::to_string(state.roots[kind]), detail, 680,
                 y, 520, 80, state.light >= cost, 0);
  }

  scene.button("skills", P.skills_text,
               game::replace(P.seeds_text, "{}", std::to_string(state.seeds)), 680, 540, 250, 72,
               true, 0);
  scene.button("replant", P.replant_text,
               game::replace(P.lifetime_text, "{}", game::number(state.lifetime)), 950, 540, 250,
               72, true, 0);
  scene.label(game::replace(game::replace(P.status_text, "{replants}",
                                          std::to_string(state.replants)),
                            "{time}", game::clock_text(state.elapsed_ms)),
              1200, 680, 20, game::kDim, 2);
  return scene;
}

auto grow(glow::GameStateT& state, double ms) -> void {
  const double gained = income(state) * ms / 1000;
  state.light += gained;
  state.lifetime += gained;
  state.total += gained;
  state.elapsed_ms += static_cast<uint64_t>(ms);
}

// The Bloom was bought: the plant flowers, then the game leaves for the end.
auto bloom(glow::GameStateT& state, game::Page& page) -> bool {
  game::Stopwatch clock;
  double shown = 0;
  bool first = true;
  while (shown < P.bloom_seconds * 1000) {
    auto scene = scene_of(state);
    scene.value.buttons.clear();
    scene.value.bloom = static_cast<float>(shown / (P.bloom_seconds * 1000));
    if (first) scene.fx(3, 320, 300);
    first = false;
    page.show(scene);
    const auto event = page.next(50);
    const double ms = clock.lap();
    shown += ms;
    state.elapsed_ms += static_cast<uint64_t>(ms);
    if (event.kind == game::Event::kClosed || event.kind == game::Event::kCancelled) return false;
  }
  return true;
}

}  // namespace

auto run(const Card::Inputs& inputs) -> Card::Output {
  auto turn = game::own_turn(inputs.turn);
  auto& state = *turn.state;
  state.roots.resize(P.root_costs.size(), 0);
  game::Page page(game::session_of(*turn.view));
  Card::Output output;

  if (state.bloom) {
    if (!bloom(state, page)) return output;
    page.leave();
    output.bloom = std::move(turn);
    return output;
  }

  game::Stopwatch clock;
  double since_scene = 1e9;
  std::vector<std::unique_ptr<glow::screen::FxT>> fx;
  while (true) {
    if (since_scene >= P.scene_ms || !fx.empty()) {
      auto scene = scene_of(state);
      scene.value.fx = std::move(fx);
      fx.clear();
      page.show(scene);
      since_scene = 0;
    }
    const auto event = page.next(P.scene_ms);
    const double ms = clock.lap();
    grow(state, ms);
    since_scene += ms;
    if (event.kind == game::Event::kClosed || event.kind == game::Event::kCancelled)
      return output;
    if (event.kind != game::Event::kButton) continue;

    if (event.button == "sprout") {
      const double gained = click_power(state);
      state.light += gained;
      state.lifetime += gained;
      state.total += gained;
      ++state.clicks;
      since_scene = P.scene_ms;
    } else if (event.button.starts_with("root")) {
      size_t kind = 0;
      const std::string_view index = std::string_view(event.button).substr(4);
      const auto end = index.data() + index.size();
      const auto [parsed, error] = std::from_chars(index.data(), end, kind);
      if (error != std::errc{} || parsed != end) continue;
      if (kind >= state.roots.size() || !known(state, kind)) continue;
      const double cost = root_cost(state, kind);
      if (state.light < cost) continue;
      state.light -= cost;
      ++state.roots[kind];
      auto& item = *fx.emplace_back(std::make_unique<glow::screen::FxT>());
      item.kind = 1;
      item.x = 1180;
      item.y = 170 + 96 * static_cast<float>(kind);
    } else if (event.button == "skills" || event.button == "replant") {
      page.leave();
      (event.button == "skills" ? output.skills : output.replant) = std::move(turn);
      return output;
    }
  }
}
