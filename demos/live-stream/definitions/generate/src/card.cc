#include <tsugumi/sdk/card.hh>

#include <chrono>
#include <cstdint>
#include <thread>

#include "_inputs_generated.h"
#include "_outputs_generated.h"

using Merged = inputs::state::tsugumi::merge::Merged;
using Arrival = inputs::state::tsugumi::merge::Arrival;

// Deterministic step from (seed, index): splitmix64 -> uniform in [-1, 1).
static double noise(uint64_t seed, uint64_t index) {
  uint64_t z = seed + 0x9e3779b97f4a7c15ull * (index + 1);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  z ^= z >> 31;
  return static_cast<double>(z >> 11) / 4503599627370496.0 - 1.0;
}

template <class S>
static void step(tsugumi::sdk::Context& ctx, const S* s) {
  // The sampling interval is the purpose of this wait, not a progress effect.
  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(s->interval_ms());
  while (std::chrono::steady_clock::now() < until) {
    if (ctx.cancelled()) throw tsugumi::sdk::Failure("cancelled", "cancelled while waiting for the next sample");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  double value = s->value() + noise(s->seed(), s->index());
  {
    flatbuffers::FlatBufferBuilder b;
    b.Finish(outputs::sample::live::CreateSample(b, s->index(), value));
    ctx.output("sample", b);
  }
  if (s->index() + 1 >= s->samples()) return;  // no feedback: the stream ends
  flatbuffers::FlatBufferBuilder b;
  b.Finish(outputs::next::live::CreateState(b, s->index() + 1, s->samples(), s->interval_ms(), s->seed(), value));
  ctx.output("next", b);
}

void run(tsugumi::sdk::Context& ctx) {
  const auto* m = ctx.input<Merged>("state");
  switch (m->value_type()) {
    case Arrival::init: return step(ctx, m->value_as_init());
    case Arrival::next: return step(ctx, m->value_as_next());
    default: throw tsugumi::sdk::Failure("output_invalid", "unexpected arrival");
  }
}
TSUGUMI_CARD(run)
