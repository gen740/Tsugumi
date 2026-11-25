#include "game.hh"

// The result of a game that bloomed, from its last state alone.
auto run(const Card::Inputs& inputs) -> Card::Output {
  auto turn = game::own_turn(inputs.turn);
  const auto& state = *turn.state;
  Card::Output output;
  auto& summary = output.summary.emplace();
  summary.minutes = static_cast<double>(state.elapsed_ms) / 60000.0;
  summary.replants = state.replants;
  summary.clicks = state.clicks;
  summary.total = state.total;
  summary.nodes = static_cast<uint32_t>(std::count(state.nodes.begin(), state.nodes.end(), 1));
  // The first rating whose minute limit the game beat; the last one otherwise.
  const auto& limits = Card::params.rating_minutes;
  size_t rating = 0;
  while (rating < limits.size() && summary.minutes > limits[rating]) ++rating;
  summary.rating = Card::params.ratings.at(std::min(rating, Card::params.ratings.size() - 1));
  summary.turn = std::make_unique<glow::TurnT>(std::move(turn));
  return output;
}
