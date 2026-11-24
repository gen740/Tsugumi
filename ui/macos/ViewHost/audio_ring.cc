#include <tsugumi/tsugumi_view.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <new>

#include "tsugumi_view_host.h"

namespace {

constexpr uint32_t kCapacity = 4096;
constexpr uint32_t kAhead = 2048;
constexpr uint32_t kChunk = 512;

}  // namespace

struct tsugumi_view_audio_ring {
  std::array<std::array<float, 2>, kCapacity> buffer{};
  std::atomic<uint64_t> clock_sequence{0};
  std::atomic<uint64_t> read{0};
  std::atomic<uint64_t> written{0};
  std::atomic<double> heard_at_read{0};
  std::atomic<double> latency{0};
};

static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);

auto tsugumi_view_audio_ring_create() -> tsugumi_view_audio_ring* {
  return new (std::nothrow) tsugumi_view_audio_ring;
}

auto tsugumi_view_audio_ring_destroy(tsugumi_view_audio_ring* ring) -> void {
  delete ring;
}

auto tsugumi_view_audio_ring_fill(tsugumi_view_audio_ring* ring,
                                  tsugumi_view_host* host) -> int32_t {
  // A render callback's timestamp names the first output sample. The read
  // cursor and its timestamp move together; keep the snapshot consistent.
  const auto sequence = ring->clock_sequence.load();
  const auto read = ring->read.load();
  const auto heard = ring->heard_at_read.load();
  if (!heard || (sequence & 1) || ring->clock_sequence.load() != sequence)
    return 0;
  const auto write =
      std::max(ring->written.load(std::memory_order_relaxed), read);
  if (write >= read + kAhead) return 0;
  const auto frames =
      static_cast<uint32_t>(std::min<uint64_t>(kChunk, read + kAhead - write));
  const auto time =
      heard + static_cast<double>(write - read) / TSUGUMI_VIEW_SAMPLE_RATE;
  std::array<float, 2 * kChunk> chunk;
  const auto result = tsugumi_view_host_audio(host, frames, time, chunk.data());
  if (result < 0) return result;
  for (uint32_t i = 0; i < frames; ++i) {
    ring->buffer[(write + i) % kCapacity] = {chunk[i * 2], chunk[i * 2 + 1]};
  }
  ring->written.store(write + frames, std::memory_order_release);
  return 0;
}

auto tsugumi_view_audio_ring_set_latency(tsugumi_view_audio_ring* ring,
                                         double seconds) -> void {
  ring->latency.store(seconds, std::memory_order_relaxed);
}

auto tsugumi_view_audio_ring_read(tsugumi_view_audio_ring* ring, float* left,
                                  float* right, uint32_t frames,
                                  double render_time) -> void {
  const auto read = ring->read.load(std::memory_order_relaxed);
  const auto write = ring->written.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < frames; ++i) {
    const auto position = read + i;
    const auto pair = position < write ? ring->buffer[position % kCapacity]
                                       : std::array<float, 2>{};
    left[i] = pair[0];
    right[i] = pair[1];
  }
  ring->clock_sequence.fetch_add(1);
  const auto next = render_time +
                    ring->latency.load(std::memory_order_relaxed) +
                    static_cast<double>(frames) / TSUGUMI_VIEW_SAMPLE_RATE;
  ring->heard_at_read.store(next);
  ring->read.store(read + frames);
  ring->clock_sequence.fetch_add(1);
}
