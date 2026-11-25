#include <card.hh>

static auto arrival_value(const Card::Inputs& inputs) -> uint64_t {
  return std::visit([](const auto& value) -> uint64_t {
    if constexpr (requires { value->seed(); }) return value->seed();
    else return value->value();
  }, inputs.arrival);
}

// Emits nothing at 1 (the loop ends); otherwise the value and its parity.
auto run(const Card::Inputs& inputs) -> Card::Output {
  uint64_t n = arrival_value(inputs);
  if (n == 0) throw tsugumi::Failure(Tsugumi::Data::FailureKind::output_invalid, "Collatz is defined for positive integers");
  Card::Output out;
  if (n == 1) return out;
  out.value.emplace().value = n;
  return out;
}
