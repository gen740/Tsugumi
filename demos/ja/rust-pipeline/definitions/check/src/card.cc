#include <card.hh>

auto run(const Card::Inputs& inputs, tsugumi::Report& report) -> Card::Output {
  report.text(std::to_string(inputs.number.value));
  return {};
}
