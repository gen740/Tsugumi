#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace tsugumi {

using ArtifactIdBytes = std::array<uint8_t, 16>;

// Big-endian words so binary order agrees with invocation and counter order.
inline auto make_artifact_id(uint64_t invocation, uint64_t counter)
    -> ArtifactIdBytes {
  ArtifactIdBytes id;
  for (int i = 7; i >= 0; --i) {
    id[i] = static_cast<uint8_t>(invocation);
    id[i + 8] = static_cast<uint8_t>(counter);
    invocation >>= 8;
    counter >>= 8;
  }
  return id;
}

inline auto artifact_id_word(std::span<const uint8_t, 8> bytes) -> uint64_t {
  uint64_t value = 0;
  for (auto byte : bytes) value = (value << 8) | byte;
  return value;
}

inline auto artifact_sequence(std::span<const uint8_t, 16> id) -> uint64_t {
  return artifact_id_word(id.first<8>());
}

inline auto artifact_counter(std::span<const uint8_t, 16> id) -> uint64_t {
  return artifact_id_word(id.last<8>());
}

// The wire verifier and history both require exactly 16 bytes.
inline auto artifact_id_bytes(std::string_view id)
    -> std::span<const uint8_t, 16> {
  return std::span<const uint8_t, 16>(
      reinterpret_cast<const uint8_t*>(id.data()), 16);
}

}  // namespace tsugumi
