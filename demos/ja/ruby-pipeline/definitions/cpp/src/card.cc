#include <card.hh>
#include <string>

auto run(const Card::Inputs& inputs) -> Card::Output {
  card::display::send(card::display::Markdown{"C++ received " + std::to_string(inputs.number->value())});
  return {};
}
