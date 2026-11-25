#include <card.hh>

#include <string>
#include <vector>

static auto arrival_value(const Card::Inputs& inputs) -> uint64_t {
  return std::visit([](const auto& value) -> uint64_t {
    if constexpr (requires { value->seed(); }) return value->seed();
    else return value->value();
  }, inputs.arrival);
}

// This Card sets reuseWorker: true, so the worker stays across invocations and runs; each run starts empty.
static std::vector<uint64_t> values;

auto run(const Card::Inputs& inputs) -> Card::Output {
  if (std::visit([](const auto& value) { return requires { value->seed(); }; },
                 inputs.arrival)) {
    values.clear();
  }
  uint64_t n = arrival_value(inputs);
  values.push_back(n);
  std::string sequence = "Sequence:";
  for (const auto value : values) sequence += " " + std::to_string(value);
  card::display::send(card::display::Markdown{sequence});
  Card::Output out;
  if (n == 1) out.sequence.emplace().values = values;
  return out;
}
