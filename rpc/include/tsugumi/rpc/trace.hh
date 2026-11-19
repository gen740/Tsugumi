#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tsugumi::trace {

enum class Mode { Off, Trace, Capture };
enum class Direction { Request, Reply, Event, Notification };

struct Metadata {
  Direction direction;
  std::string_view type;
  std::optional<uint64_t> request_id = {};
  std::string_view failure = {};
};

struct Content {
  std::string json;
  const char* omission = "";
};

struct Codec {
  auto (*describe)(std::string_view, bool outgoing) -> Metadata;
  auto (*decode)(std::string_view) -> Content;
};

// A link is owned by one framing loop or synchronous exchange.
struct Link {
  uint64_t id = 0;
};

struct Entry {
  uint64_t sequence;
  uint64_t unix_micros;
  std::string_view from;
  std::string_view to;
  Direction direction;
  std::string type;
  std::optional<uint64_t> request_id;
  uint32_t bytes;
  std::optional<uint64_t> elapsed_micros;
  std::string failure;
  Content content;
};

struct Snapshot {
  Mode mode;
  std::vector<Entry> entries;
};

// Keep enough context to debug a burst, with a separate 4 MiB limit so that
// capture cannot turn frames containing files into an unbounded memory log.
inline constexpr size_t kMaxEntries = 2048;
inline constexpr size_t kMaxContentBytes = 4u << 20;
inline constexpr size_t kMaxFrameToDecode = 64u << 10;
inline constexpr size_t kMaxDecodedBytes = 128u << 10;

inline constexpr char kLaunchVariable[] = "TSUGUMI_RPC_TRACE";
auto LaunchEnabled() -> bool;
auto SetMode(Mode mode) -> void;
auto CurrentMode() -> Mode;
auto Read() -> Snapshot;

namespace detail {
extern std::atomic<uint64_t> mode_epoch;
auto Record(uint64_t epoch, Link& link, const char* from, const char* to,
            bool outgoing, std::string_view bytes, const Codec* codec) noexcept
    -> void;
auto Record(uint64_t epoch, Link& link, const char* from, const char* to,
            bool outgoing, const std::string& bytes,
            const Codec* codec) noexcept -> void;
}  // namespace detail

// Endpoint names must refer to static strings. The off path is one relaxed
// load.
template <class Frame = std::string_view>
inline auto Observe(Link* link, const char* from, const char* to, bool outgoing,
                    const Frame& bytes, const Codec* codec) noexcept -> void {
  const auto epoch = detail::mode_epoch.load(std::memory_order_relaxed);
  if ((epoch & 3) != 0) [[unlikely]]
    if (link) detail::Record(epoch, *link, from, to, outgoing, bytes, codec);
}

template <class Frame = std::string_view>
inline auto Observe(Link& link, const char* from, const char* to, bool outgoing,
                    const Frame& bytes, const Codec* codec) noexcept -> void {
  Observe(&link, from, to, outgoing, bytes, codec);
}

}  // namespace tsugumi::trace
