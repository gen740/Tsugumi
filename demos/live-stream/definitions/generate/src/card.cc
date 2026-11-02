#include <card.hh>

#include <chrono>
#include <cstdint>
#include <thread>

// Deterministic step from (seed, index): splitmix64 -> uniform in [-1, 1).
static double noise(uint64_t seed, uint64_t index) {
  uint64_t z = seed + 0x9e3779b97f4a7c15ull * (index + 1);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  z ^= z >> 31;
  return static_cast<double>(z >> 11) / 4503599627370496.0 - 1.0;
}

template <class S>
static Card::Output step(const S& s) {
  // The sampling interval is the purpose of this wait, not a progress effect.
  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(s.interval_ms);
  while (std::chrono::steady_clock::now() < until) {
    if (tsugumi::cancelled()) throw tsugumi::Failure("cancelled", "cancelled while waiting for the next sample");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  double value = s.value + noise(s.seed, s.index);
  Card::Output out;
  auto& sample = out.sample.emplace();
  sample.index = s.index;
  sample.value = value;
  if (s.index + 1 >= s.samples) return out;  // no feedback: the stream ends
  auto& next = out.next.emplace();
  next.index = s.index + 1;
  next.samples = s.samples;
  next.interval_ms = s.interval_ms;
  next.seed = s.seed;
  next.value = value;
  return out;
}

// The loop delivers a merge union tagged with the arrival port.
auto run(Card::Input& input) -> Card::Output {
  const auto& u = input.state.value;
  if (const auto* s = u.Asinit()) return step(*s);
  if (const auto* s = u.Asnext()) return step(*s);
  throw tsugumi::Failure("output_invalid", "unexpected arrival");
}
