#pragma once

#include <cstdint>

namespace tsugumi::protocol {

inline constexpr uint32_t kMinLogRangeBytes = 4;
inline constexpr uint32_t kMaxLogRangeBytes = 1u << 20;

}  // namespace tsugumi::protocol
