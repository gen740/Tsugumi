#include <card.hh>
auto run(Card::Input&) -> Card::Output {
  Card::Output out;
  auto& readings = out.numbers.emplace();
  readings.values = {1.25, 2.5, 3.75};
  readings.label = "Sensor readings";
  return out;
}
