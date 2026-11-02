#include <card.hh>

auto run(Card::Input& input) -> Card::Output {
  Card::Output out;
  out.next.emplace().value = input.number.value / 2;
  return out;
}
