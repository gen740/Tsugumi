#include "tsugumi/rpc/trace.hh"

#include <chrono>
#include <deque>
#include <map>
#include <mutex>

namespace tsugumi::trace {
namespace {
using Clock = std::chrono::steady_clock;
using Key = std::pair<uint64_t, uint64_t>;

struct Recorder {
  std::mutex mutex;
  std::deque<Entry> entries;
  std::map<Key, Clock::time_point> pending;
  size_t content_bytes = 0;
  uint64_t sequence = 0;
  uint64_t next_link = 0;
};

auto State() -> Recorder& {
  static Recorder state;
  return state;
}

auto DecodeMode(uint64_t epoch) -> Mode { return static_cast<Mode>(epoch & 3); }
}  // namespace

constinit std::atomic<uint64_t> detail::mode_epoch{0};

auto CurrentMode() -> Mode {
  return DecodeMode(detail::mode_epoch.load(std::memory_order_relaxed));
}

auto SetMode(Mode mode) -> void {
  auto& state = State();
  std::lock_guard lock(state.mutex);
  const auto epoch = detail::mode_epoch.load(std::memory_order_relaxed);
  if (DecodeMode(epoch) == mode) return;
  if (mode != Mode::Capture) {
    for (auto& entry : state.entries) entry.content = {};
    state.content_bytes = 0;
  }
  if (mode == Mode::Off) state.pending.clear();
  detail::mode_epoch.store(
      ((epoch & ~uint64_t{3}) + 4) | static_cast<uint64_t>(mode),
      std::memory_order_relaxed);
}

auto Read() -> Snapshot {
  auto& state = State();
  std::lock_guard lock(state.mutex);
  return {CurrentMode(), {state.entries.begin(), state.entries.end()}};
}

auto detail::Record(uint64_t epoch, Link& link, const char* from,
                    const char* to, bool outgoing, std::string_view bytes,
                    const Codec* codec) noexcept -> void {
  if (!codec) return;
  try {
    const auto now = Clock::now();
    const auto metadata = codec->describe(bytes, outgoing);
    if (metadata.type.empty()) return;
    const auto wall = std::chrono::system_clock::now().time_since_epoch();
    using Micros = std::chrono::microseconds;
    Entry entry{.sequence = 0,
                .unix_micros = static_cast<uint64_t>(
                    std::chrono::duration_cast<Micros>(wall).count()),
                .from = from,
                .to = to,
                .direction = metadata.direction,
                .type = std::string(metadata.type),
                .request_id = metadata.request_id,
                .bytes = static_cast<uint32_t>(bytes.size()),
                .elapsed_micros = {},
                .failure = std::string(metadata.failure.substr(0, 64)),
                .content = {}};
    if (DecodeMode(epoch) == Mode::Capture) {
      entry.content = bytes.size() <= kMaxFrameToDecode
                          ? codec->decode(bytes)
                          : Content{.json = {}, .omission = "size"};
      if (entry.content.json.size() > kMaxDecodedBytes)
        entry.content = {.json = {}, .omission = "size"};
    }
    auto& state = State();
    std::lock_guard lock(state.mutex);
    if (mode_epoch.load(std::memory_order_relaxed) != epoch) return;
    if (link.id == 0) link.id = ++state.next_link;
    const Key key{link.id, metadata.request_id.value_or(0)};
    if (metadata.direction == Direction::Request) {
      if (state.pending.size() == kMaxEntries)
        state.pending.erase(state.pending.begin());
      state.pending.insert_or_assign(key, now);
    } else if (metadata.direction == Direction::Reply) {
      auto pending = state.pending.extract(key);
      if (!pending.empty())
        entry.elapsed_micros =
            std::chrono::duration_cast<Micros>(now - pending.mapped()).count();
    }
    if (state.entries.size() == kMaxEntries) {
      state.content_bytes -= state.entries.front().content.json.size();
      state.entries.pop_front();
    }
    for (auto& old : state.entries) {
      if (state.content_bytes + entry.content.json.size() <= kMaxContentBytes)
        break;
      if (old.content.json.empty()) continue;
      state.content_bytes -= old.content.json.size();
      old.content = {.json = {}, .omission = "evicted"};
    }
    state.content_bytes += entry.content.json.size();
    entry.sequence = ++state.sequence;
    state.entries.push_back(std::move(entry));
  } catch (...) {
    // Debug recording must never interrupt traffic.
  }
}

// Defer inspecting a string's storage and length to the enabled path.
auto detail::Record(uint64_t epoch, Link& link, const char* from,
                    const char* to, bool outgoing, const std::string& bytes,
                    const Codec* codec) noexcept -> void {
  Record(epoch, link, from, to, outgoing, std::string_view(bytes), codec);
}

}  // namespace tsugumi::trace
