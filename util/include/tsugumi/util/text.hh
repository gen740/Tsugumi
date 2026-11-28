// Text helpers shared by the core and the worker.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tsugumi::text {

// Returns true when `s` is valid UTF-8 (no surrogates, no overlongs).
bool valid_utf8(std::string_view s);

// Incremental UTF-8 decoder: emits complete characters only and replaces
// invalid sequences with U+FFFD, so chunk boundaries never split or lose text.
class Utf8Decoder {
 public:
  auto feed(std::string_view bytes) -> std::string;
  auto finish() -> std::string;

 private:
  std::string pending_;
};

auto replace_invalid_utf8(std::string text) -> std::string;
// Text for a person to read from what a program wrote for a terminal: removes
// escape sequences (CSI such as colours and cursor moves, OSC such as links
// and titles, DCS/SOS/PM/APC strings, two-byte escapes), their C1 forms, and
// every other control character except tab and line feed. Other UTF-8 text is
// kept as it is.
auto strip_terminal_controls(std::string_view text) -> std::string;
// For portable run metadata. Removes credentials and store URL settings.
auto public_cache_endpoint(std::string url) -> std::string;

struct PatchRangeError : std::invalid_argument {
  using std::invalid_argument::invalid_argument;
};
struct PatchBoundaryError : PatchRangeError {
  using PatchRangeError::PatchRangeError;
};

// Applies ascending, non-overlapping edits at UTF-8 boundaries. The source
// must outlive the applier.
class PatchApplier {
 public:
  explicit PatchApplier(std::string_view source) : source_(source) {}

  auto append(uint64_t offset, uint64_t remove, std::string_view text) -> void;
  auto finish() && -> std::string;

 private:
  std::string_view source_;
  std::string result_;
  size_t cursor_ = 0;
};

}  // namespace tsugumi::text
