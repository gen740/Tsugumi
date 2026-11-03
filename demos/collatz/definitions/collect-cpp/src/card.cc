#include <card.hh>

#include <string>
#include <vector>

// The loop delivers a merge union tagged with the arrival port.
static uint64_t arrival_value(const Card::Input& input) {
  const auto& u = input.arrival.value;
  if (const auto* v = u.Asinit()) return v->value;
  if (const auto* v = u.Ashalf()) return v->value;
  if (const auto* v = u.Astriple()) return v->value;
  throw tsugumi::Failure("output_invalid", "unexpected arrival");
}

// The worker stays across runs (reuseWorker); each run starts empty.
static std::vector<uint64_t> values;

auto onRunStart() -> void { values.clear(); }

auto run(Card::Input& input) -> Card::Output {
  if (input.arrival.value.Asinit()) values.clear();
  uint64_t n = arrival_value(input);
  values.push_back(n);
  std::string text = std::to_string(values.size()) + " values:";
  for (uint64_t v : values) text += " " + std::to_string(v);
  tsugumi::visualization::text(text);
  Card::Output out;
  if (n == 1) out.sequence.emplace().values = values;
  return out;
}
