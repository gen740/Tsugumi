#include "page.hh"

// Asks the player to confirm the replant that replant/apply worked out.
auto run(const Card::Inputs& inputs) -> Card::Output {
  const auto& P = Card::params;
  const auto* offer = inputs.offer;
  auto turn = game::own_turn(offer->turn());
  game::Page page(game::session_of(*turn.view));
  game::Stopwatch clock;
  Card::Output output;

  const auto& state = *turn.state;
  game::Scene scene("replant");
  scene.label(P.title, 640, 120, 44, game::kText, 1);
  scene.label(game::replace(P.lifetime_text, "{}", game::number(state.lifetime)), 640, 200, 26,
              game::kGold, 1);
  if (offer->ready()) {
    scene.counter(P.gain_text, offer->seeds(), 640, 300, 40, game::kGreen, 1);
    scene.label(P.loss_text, 640, 360, 22, game::kDim, 1);
  } else {
    scene.label(P.not_yet_text, 640, 300, 30, game::kText, 1);
  }
  scene.label(game::replace(P.next_text, "{}", game::number(offer->next_seed_at())), 640, 410,
              22, game::kDim, 1);
  scene.button("replant", P.confirm_text, "", 400, 500, 220, 72, offer->ready(), 1);
  scene.button("back", P.back_text, "", 660, 500, 220, 72, true, 0);
  scene.value.plant = 0.3f;
  page.show(scene);

  bool replanting = false;
  double since = 0;
  while (true) {
    const auto event = page.next();
    const double ms = clock.lap();
    turn.state->elapsed_ms += static_cast<uint64_t>(ms);
    since += ms;
    if (event.kind == game::Event::kClosed || event.kind == game::Event::kCancelled)
      return output;
    if (replanting && since >= P.chime_ms) break;
    if (replanting || event.kind != game::Event::kButton) continue;
    if (event.button == "back") break;
    if (event.button == "replant" && offer->ready()) {
      // The chime plays while the old plant fades.
      replanting = true;
      since = 0;
      game::Scene chime("replant");
      chime.label(P.done_text, 640, 300, 40, game::kGreen, 1);
      chime.fx(2, 640, 360);
      page.show(chime);
    }
  }
  page.leave();
  if (replanting) {
    auto next = game::own_state(offer->next());
    next->elapsed_ms = turn.state->elapsed_ms;
    output.done = game::make_turn(std::move(next), *turn.view);
  } else {
    output.done = std::move(turn);
  }
  return output;
}
