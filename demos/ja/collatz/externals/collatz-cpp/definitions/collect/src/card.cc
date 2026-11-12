#include <card.hh>

#include <string>
#include <vector>

// The loop delivers a merge union tagged with the arrival port; every port
// carries a Number.
static uint64_t arrival_value(const Card::Input& input) {
  return Card::visit(input.arrival.value, [](const auto& v) { return v.value; });
}

// This Card sets reuseWorker: true, so the worker stays across invocations and runs; each run starts empty.
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
