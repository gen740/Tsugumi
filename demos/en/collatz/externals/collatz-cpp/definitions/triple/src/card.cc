#include <card.hh>

#include <limits>
#include <string>

auto run(Card::Input& input) -> Card::Output {
  uint64_t n = input.number.value;
  constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
  if (n > (kMax - 1) / 3)
    throw tsugumi::Failure("output_invalid", "integer overflow: 3*" + std::to_string(n) + "+1 exceeds uint64");
  Card::Output out;
  out.next.emplace().value = 3 * n + 1;
  return out;
}
