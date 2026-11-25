#include "game.hh"

// Replanting: light and roots go, seeds come from the light grown since the
// last replant. Works out the replanted state for the page to confirm.
auto run(const Card::Inputs& inputs) -> Card::Output {
  const auto& P = Card::params;
  auto turn = game::own_turn(inputs.turn);
  const auto& state = *turn.state;
  const double boost = state.boosts->seeds;

  Card::Output output;
  auto& offer = output.offer.emplace();
  offer.seeds = state.lifetime >= P.replant_at
                    ? static_cast<uint32_t>(std::floor(std::sqrt(state.lifetime / P.replant_at) * boost))
                    : 0;
  offer.ready = offer.seeds > 0;
  // The light this life needs for one more seed.
  const double more = (offer.seeds + 1) / boost;
  offer.next_seed_at = std::max(P.replant_at, P.replant_at * more * more);

  offer.next = game::copy_state(state);
  offer.next->boosts = std::make_unique<glow::BoostsT>(*state.boosts);
  if (offer.ready) {
    offer.next->light = 0;
    offer.next->lifetime = 0;
    std::fill(offer.next->roots.begin(), offer.next->roots.end(), 0);
    offer.next->seeds += offer.seeds;
    offer.next->replants += 1;
  }
  offer.turn = std::make_unique<glow::TurnT>(std::move(turn));
  return output;
}
