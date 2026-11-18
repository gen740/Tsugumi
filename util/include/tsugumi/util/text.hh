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
