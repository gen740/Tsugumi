#include "card.hh"

#include <array>

auto run(const Card::Inputs&) -> Card::Output {
  using namespace card::display;
  constexpr std::array<uint8_t, 1> ready{1};
  constexpr std::array<uint8_t, 1> accepted{0};

  while (!card::cancelled()) {
    // The view may be opened after the run starts.
    send_raw("seed", ready);
    const auto reply = recv("seed", 1000);
    if (reply.status == ReceiveStatus::kCancelled) break;
    if (reply.status != ReceiveStatus::kMessage || reply.bytes.size() != 1 ||
        reply.bytes.front() > 20)
      continue;

    send_raw("seed", accepted);
    Card::Output output;
    auto& state = output.state.emplace();
    state.index = Card::params.index;
    state.samples = Card::params.samples;
    state.interval_ms = Card::params.interval_ms;
    state.seed = Card::params.seed;
    state.value = static_cast<double>(reply.bytes.front()) - 10.0;
    return output;
  }
  throw tsugumi::Failure(Tsugumi::Data::FailureKind::cancelled, "cancelled while waiting for the seed");
}
