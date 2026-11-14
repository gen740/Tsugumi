// The core chooses the recording budget; local and remote runners enforce it.
#pragma once
#include <cstdint>
namespace tsugumi {
inline constexpr uint64_t kInvocationLogLimitBytes = uint64_t{8} << 20;
}  // namespace tsugumi
