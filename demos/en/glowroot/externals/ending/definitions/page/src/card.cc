#include "page.hh"

// The last screen: how the game went. The result also goes to the output
// port, so it stays in the run's record after the window closes.
auto run(const Card::Inputs& inputs) -> Card::Output {
  const auto& P = Card::params;
  const auto* summary = inputs.summary;
  auto turn = game::own_turn(summary->turn());
  game::Page page(game::session_of(*turn.view));

  game::Scene scene("ending");
  scene.value.bloom = 1;
  scene.value.plant = 1;
  scene.label(P.title, 640, 110, 64, game::kGold, 1);
  scene.label(summary->rating() ? summary->rating()->str() : "", 640, 170, 28, game::kText, 1);
  scene.label(game::replace(P.time_text, "{}",
                            game::clock_text(static_cast<uint64_t>(summary->minutes() * 60000))),
              640, 260, 26, game::kText, 1);
  scene.counter(P.total_text, summary->total(), 640, 310, 26, game::kGold, 1);
  scene.counter(P.replants_text, summary->replants(), 640, 360, 26, game::kGreen, 1);
  scene.counter(P.nodes_text, summary->nodes(), 640, 410, 26, game::kGreen, 1);
  scene.counter(P.clicks_text, static_cast<double>(summary->clicks()), 640, 460, 26, game::kDim,
                1);
  scene.label(P.thanks_text, 640, 560, 24, game::kText, 1);
  scene.button("finish", P.finish_text, "", 540, 610, 200, 64, true, 1);
  scene.fx(3, 640, 300);
  page.show(scene);

  while (true) {
    const auto event = page.next();
    if (event.kind == game::Event::kCancelled) break;
    if (event.kind == game::Event::kClosed) break;
    if (event.kind == game::Event::kButton && event.button == "finish") break;
  }
  if (!page.closed()) page.close();
  Card::Output output;
  auto& result = output.result.emplace();
  result.minutes = summary->minutes();
  result.replants = summary->replants();
  result.nodes = summary->nodes();
  result.clicks = summary->clicks();
  result.total = summary->total();
  if (summary->rating()) result.rating = summary->rating()->str();
  result.turn = std::make_unique<glow::TurnT>(std::move(turn));
  return output;
}
