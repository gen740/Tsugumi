// Text helpers shared by the core and the worker.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tsugumi::text {

// Returns true when `s` is valid UTF-8 (no surrogates, no overlongs).
bool valid_utf8(std::string_view s);

// Applies byte edits to a source that must outlive the applier. Callers
// validate that edits are ascending, non-overlapping and in range.
class PatchApplier {
 public:
  explicit PatchApplier(std::string_view source) : source_(source) {}

  auto cursor() const -> size_t { return cursor_; }
  auto append(size_t offset, size_t remove, std::string_view text) -> void;
  auto finish() && -> std::string;

 private:
  std::string_view source_;
  std::string result_;
  size_t cursor_ = 0;
};

}  // namespace tsugumi::text
