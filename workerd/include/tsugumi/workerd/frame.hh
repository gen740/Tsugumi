#pragma once

#include <array>
#include <cstdint>

namespace tsugumi::workerd {
// Both workers use little-endian uint32 framing, independently of the carrier.
auto inline encode_length(uint32_t size) -> std::array<char, 4> {
  std::array<char, 4> bytes{};
  for (unsigned i = 0; i < 4; ++i)
    bytes[i] = static_cast<char>(size >> (8 * i));
  return bytes;
}
auto inline decode_length(const char* bytes) -> uint32_t {
  uint32_t size = 0;
  for (unsigned i = 0; i < 4; ++i)
    size |= uint32_t{static_cast<uint8_t>(bytes[i])} << (8 * i);
  return size;
}
}  // namespace tsugumi::workerd
