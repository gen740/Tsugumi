#include <card.hh>

auto run(const Card::Inputs& inputs) -> Card::Output {
  Card::Output out;
  out.next.emplace().value = inputs.number.value / 2;
  return out;
}
