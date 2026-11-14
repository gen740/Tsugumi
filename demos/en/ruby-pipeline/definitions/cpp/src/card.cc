#include <card.hh>
#include <string>

auto run(const Card::Inputs& inputs, tsugumi::Report& report) -> Card::Output {
  report.text("C++ received " + std::to_string(inputs.number.value));
  return {};
}
