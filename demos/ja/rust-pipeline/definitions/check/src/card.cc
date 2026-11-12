#include <card.hh>

auto run(Card::Input& input) -> Card::Output {
  tsugumi::visualization::text(std::to_string(input.number.value));
  return {};
}
