#include "tsugumi/util/text.hh"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace tsugumi::text {

auto PatchApplier::append(uint64_t offset, uint64_t remove,
                          std::string_view text) -> void {
  if (offset < cursor_)
    throw PatchRangeError("patches must be ascending and non-overlapping");
  if (offset > source_.size() || remove > source_.size() - offset)
    throw PatchRangeError("patch exceeds the source size");
  const auto boundary = [&](size_t position) {
    return position == 0 || position == source_.size() ||
           (static_cast<unsigned char>(source_[position]) & 0xc0) != 0x80;
  };
  if (!boundary(offset) || !boundary(offset + remove))
    throw PatchBoundaryError("patch is not on a UTF-8 boundary");
  result_.append(source_.substr(cursor_, offset - cursor_));
  result_.append(text);
  cursor_ = offset + remove;
}

auto PatchApplier::finish() && -> std::string {
  result_.append(source_.substr(cursor_));
  return std::move(result_);
}

bool valid_utf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = s[i];
    size_t n;
    uint32_t cp;
    if (c < 0x80) {
      ++i;
      continue;
    }
    if ((c & 0xE0) == 0xC0) {
      n = 1;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      n = 2;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      n = 3;
      cp = c & 0x07;
    } else
      return false;
    if (i + n >= s.size()) return false;
    for (size_t k = 1; k <= n; ++k) {
      unsigned char cc = s[i + k];
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) ||
        (n == 3 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    i += n + 1;
  }
  return true;
}

}  // namespace tsugumi::text
