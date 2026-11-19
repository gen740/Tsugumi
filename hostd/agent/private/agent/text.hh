#pragma once

#include <algorithm>
#include <string>
#include <string_view>

namespace tsugumi::agent {
inline auto Excerpt(std::string_view text, size_t position = 0,
                    size_t limit = 240) -> std::string {
  size_t begin = position > limit / 3 ? position - limit / 3 : 0;
  while (begin && (static_cast<unsigned char>(text[begin]) & 0xc0) == 0x80)
    --begin;
  size_t end = std::min(text.size(), begin + limit);
  while (end < text.size() && end > begin &&
         (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
    --end;
  return (begin ? "…" : "") + std::string(text.substr(begin, end - begin)) +
         (end < text.size() ? "…" : "");
}
}  // namespace tsugumi::agent
