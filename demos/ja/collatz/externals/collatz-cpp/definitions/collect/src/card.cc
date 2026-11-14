#include <card.hh>

#include <string>
#include <vector>

// The loop delivers a merge union tagged with the arrival port; every port
// carries a Number.
static uint64_t arrival_value(const Card::Inputs& inputs) {
  return Card::visit(inputs.arrival.value, [](const auto& v) { return v.value; });
}

// This Card sets reuseWorker: true, so the worker stays across invocations and runs; each run starts empty.
static std::vector<uint64_t> values;

auto onRunStart(const Card::Inputs& inputs) -> void { values.clear(); }

auto run(const Card::Inputs& inputs, tsugumi::Report& report) -> Card::Output {
  if (inputs.arrival.value.Asinit()) values.clear();
  uint64_t n = arrival_value(inputs);
  values.push_back(n);
  std::string text = std::to_string(values.size()) + " values:";
  for (uint64_t v : values) text += " " + std::to_string(v);
  report.text(text);
  Card::Output out;
  if (n == 1) out.sequence.emplace().values = values;
  return out;
}
