#include "page.hh"

// Opens the game's window, shows the title and starts a new game when the
// player presses the start button.
auto run(const Card::Inputs&) -> Card::Output {
  const auto opened = card::display::view_open("screen", TSUGUMI_VIEW_WINDOW, 0,
                                               Card::params.width, Card::params.height);
  game::Page page(opened.session, opened.route);

  game::Scene scene("title");
  scene.label(Card::params.title, 640, 250, 96, game::kGold, 1);
  scene.label(Card::params.subtitle, 640, 320, 28, game::kText, 1);
  scene.button("start", Card::params.start, "", 540, 420, 200, 64, true, 1);
  scene.label(Card::params.hint, 640, 560, 22, game::kDim, 1);
  scene.value.plant = 0.12f;
  page.show(scene);

  while (true) {
    const auto event = page.next();
    if (event.kind == game::Event::kClosed || event.kind == game::Event::kCancelled)
      return {};
    if (event.kind != game::Event::kButton || event.button != "start") continue;

    auto state = std::make_unique<glow::GameStateT>();
    state->boosts = std::make_unique<glow::BoostsT>();
    state->light = Card::params.start_light;
    state->roots.assign(Card::params.root_kinds, 0);
    state->nodes.assign(Card::params.node_count, 0);
    page.leave();
    Card::Output output;
    auto& turn = output.turn.emplace();
    turn.state = std::move(state);
    turn.view = game::view_of(opened.session);
    return output;
  }
}
