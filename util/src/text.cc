#include "tsugumi/util/text.hh"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace tsugumi::text {

auto public_cache_endpoint(std::string url) -> std::string {
  if (const auto suffix = url.find_first_of("?#"); suffix != url.npos)
    url.resize(suffix);
  if (!url.starts_with("http://") && !url.starts_with("https://") &&
      !url.starts_with("s3://"))
    return {};
  const auto host = url.find("://") + 3;
  const auto slash = url.find('/', host);
  const auto at = url.rfind('@', slash);
  if (at != url.npos && at >= host) url.erase(host, at + 1 - host);
  if (url.size() == host || url[host] == '/' || url.size() > 2048) return {};
  for (unsigned char c : url)
    if (c <= 32 || c == 127) return {};
  return url;
}

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

auto Utf8Decoder::feed(std::string_view bytes) -> std::string {
  std::string in = pending_ + std::string(bytes);
  pending_.clear();
  std::string out;
  out.reserve(in.size());
  size_t i = 0;
  while (i < in.size()) {
    unsigned char c = in[i];
    size_t n;
    if (c < 0x80) {
      out += static_cast<char>(c);
      ++i;
      continue;
    }
    if ((c & 0xE0) == 0xC0)
      n = 2;
    else if ((c & 0xF0) == 0xE0)
      n = 3;
    else if ((c & 0xF8) == 0xF0)
      n = 4;
    else {
      out += "\xEF\xBF\xBD";
      ++i;
      continue;
    }
    if (i + n > in.size()) {
      // Possibly an incomplete character at the chunk end: keep it if it could
      // still be valid.
      bool ok = true;
      for (size_t k = 1; i + k < in.size(); ++k)
        ok &= (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
      if (ok) {
        pending_ = in.substr(i);
        break;
      }
      out += "\xEF\xBF\xBD";
      ++i;
      continue;
    }
    std::string_view ch(in.data() + i, n);
    if (valid_utf8(ch)) {
      out += ch;
      i += n;
    } else {
      out += "\xEF\xBF\xBD";
      ++i;
    }
  }
  return out;
}

auto Utf8Decoder::finish() -> std::string {
  std::string out = pending_.empty() ? "" : "\xEF\xBF\xBD";
  pending_.clear();
  return out;
}

auto replace_invalid_utf8(std::string text) -> std::string {
  if (valid_utf8(text)) return text;
  Utf8Decoder decoder;
  auto repaired = decoder.feed(text);
  repaired += decoder.finish();
  return repaired;
}

auto strip_terminal_controls(std::string_view text) -> std::string {
  const auto at = [&](size_t i) -> unsigned char {
    return i < text.size() ? static_cast<unsigned char>(text[i]) : 0;
  };
  // U+0080..U+009F in UTF-8.
  const auto c1 = [&](size_t i) {
    return at(i) == 0xc2 && at(i + 1) >= 0x80 && at(i + 1) <= 0x9f;
  };
  // Parameters, intermediates and a final byte. A malformed sequence ends at
  // the first byte that cannot belong to it, which is kept.
  const auto csi = [&](size_t i) {
    while (at(i) >= 0x30 && at(i) <= 0x3f) ++i;
    while (at(i) >= 0x20 && at(i) <= 0x2f) ++i;
    if (at(i) >= 0x40 && at(i) <= 0x7e) ++i;
    return i;
  };
  // A string control ends at BEL or ST (ESC \ or its C1 form). Another escape
  // also ends it and is read as itself.
  const auto string_control = [&](size_t i) {
    while (i < text.size()) {
      if (at(i) == 0x07) return i + 1;
      if (at(i) == 0x1b) return at(i + 1) == '\\' ? i + 2 : i;
      if (c1(i) && at(i + 1) == 0x9c) return i + 2;
      ++i;
    }
    return i;
  };
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    const auto c = at(i);
    if (c == 0x1b) {
      const auto next = at(i + 1);
      if (next == '[') {
        i = csi(i + 2);
      } else if (next == ']' || next == 'P' || next == 'X' || next == '^' ||
                 next == '_') {
        i = string_control(i + 2);
      } else if (next >= 0x20 && next <= 0x2f) {
        i += 2;
        while (at(i) >= 0x20 && at(i) <= 0x2f) ++i;
        if (at(i) >= 0x30 && at(i) <= 0x7e) ++i;
      } else if (next >= 0x30 && next <= 0x7e) {
        i += 2;
      } else {
        ++i;  // a lone ESC
      }
    } else if (c1(i)) {
      const auto code = at(i + 1);
      if (code == 0x9b)
        i = csi(i + 2);
      else if (code == 0x9d || code == 0x90 || code == 0x98 || code == 0x9e ||
               code == 0x9f)
        i = string_control(i + 2);
      else
        i += 2;
    } else if ((c < 0x20 && c != '\t' && c != '\n') || c == 0x7f) {
      ++i;
    } else {
      out += static_cast<char>(c);
      ++i;
    }
  }
  return out;
}

}  // namespace tsugumi::text
