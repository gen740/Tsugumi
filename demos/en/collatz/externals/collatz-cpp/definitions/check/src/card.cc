#include <card.hh>

// The loop delivers a merge union tagged with the arrival port; every port
// carries a Number.
static uint64_t arrival_value(const Card::Inputs& inputs) {
  return Card::visit(inputs.arrival.value, [](const auto& v) { return v.value; });
}

// Emits nothing at 1 (the loop ends); otherwise the value and its parity.
auto run(const Card::Inputs& inputs) -> Card::Output {
  uint64_t n = arrival_value(inputs);
  if (n == 0) throw tsugumi::Failure("output_invalid", "Collatz is defined for positive integers");
  Card::Output out;
  if (n == 1) return out;
  out.value.emplace().value = n;
  out.even.emplace().value = n % 2 == 0;
  return out;
}
