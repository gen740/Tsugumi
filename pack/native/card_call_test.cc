#include <array>
#include <cstdio>
#include <stdexcept>

#include "card_call.hh"

using tsugumi::pack::CardImage;
using tsugumi::pack::OutputPort;

namespace {

auto require(bool condition, const char* message) -> void {
  if (!condition) throw std::runtime_error(message);
}

auto rejects(auto call, std::string_view diagnostic) -> void {
  try {
    call();
  } catch (const std::runtime_error& error) {
    require(error.what() == diagnostic, error.what());
    return;
  }
  throw std::runtime_error("Unexpected Card success");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    CardImage image(argv[1]);
    const auto expected = std::array{OutputPort{"out", true}};
    tsugumi_call call{TSUGUMI_ABI_VERSION, {}, nullptr, nullptr};
    for (const bool verify : {false, true}) {
      const auto result = image.call(call, expected, verify);
      const auto* bytes = result.root().outputs()->Get(0)->bytes();
      const auto payload = result.payload();
      require(bytes->size() == 1 && bytes->Get(0) == 42, "Wrong port payload");
      require(bytes->Data() >= payload.data &&
                  bytes->Data() < payload.data + payload.size,
              "Port does not refer to the owned result");
    }
    const auto missing = std::array{OutputPort{"missing", true}};
    rejects([&] { image.call(call, missing, false); },
            "Card omitted required output missing");
    call.input.size = TSUGUMI_MESSAGE_MAX + 1;
    rejects([&] { image.call(call, expected, false); },
            "Card input exceeds 16 MiB");
    const uint8_t corrupt_fixture = 0;
    call.input = {&corrupt_fixture, 1};
    rejects([&] { image.call(call, expected, true); },
            "Invalid Card result FlatBuffer");

    call.input = {};
    tsugumi_effects effects{};
    effects.cancelled = [](void*) -> int { return 1; };
    call.effects = &effects;
    rejects([&] { image.call(call, expected, false); },
            "Card invocation cancelled");
    effects.cancelled = nullptr;
    effects.remaining_ms = [](void*) -> int64_t { return 0; };
    rejects([&] { image.call(call, expected, false); },
            "Card invocation exceeded its deadline");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
