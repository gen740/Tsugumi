#include <card.hh>
#include <string>

auto run(Card::Input& input) -> Card::Output {
  tsugumi::visualization::text("C++ received " + std::to_string(input.number.value));
  return {};
}
