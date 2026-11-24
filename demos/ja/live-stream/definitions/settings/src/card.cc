#include "card.hh"

auto run(const Card::Inputs&) -> Card::Output {
  Card::Output output;
  auto& state = output.state.emplace();
  state.index = Card::params.index;
  state.samples = Card::params.samples;
  state.interval_ms = Card::params.interval_ms;
  state.seed = Card::params.seed;
  state.value = Card::params.value;
  return output;
}
