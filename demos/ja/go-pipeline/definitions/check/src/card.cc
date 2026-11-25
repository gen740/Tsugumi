#include <card.hh>

auto run(const Card::Inputs& inputs) -> Card::Output {
  card::display::send(card::display::Markdown{std::to_string(inputs.number.value)});
  return {};
}
