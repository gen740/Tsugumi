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

}  // namespace tsugumi::text
