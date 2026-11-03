#include <card.hh>

// The loop delivers a merge union tagged with the arrival port.
static uint64_t arrival_value(const Card::Input& input) {
  const auto& u = input.arrival.value;
  if (const auto* v = u.Asinit()) return v->value;
  if (const auto* v = u.Ashalf()) return v->value;
  if (const auto* v = u.Astriple()) return v->value;
  throw tsugumi::Failure("output_invalid", "unexpected arrival");
}

// Emits nothing at 1 (the loop ends); otherwise the value and its parity.
auto run(Card::Input& input) -> Card::Output {
  uint64_t n = arrival_value(input);
  if (n == 0) throw tsugumi::Failure("output_invalid", "Collatz is defined for positive integers");
  Card::Output out;
  if (n == 1) return out;
  out.value.emplace().value = n;
  out.even.emplace().value = n % 2 == 0;
  return out;
}
