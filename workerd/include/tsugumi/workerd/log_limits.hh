#pragma once

#include <cstdint>

namespace tsugumi::remote {

inline constexpr uint64_t kMaxLogLimitBytes = uint64_t{16} << 20;
// InvocationLog::finish writes one bounded journal notice outside the budget.
inline constexpr uint64_t kLogNoticeBytes = 128;
inline constexpr uint64_t kMaxLogResultBytes = kMaxLogLimitBytes + kLogNoticeBytes;

}  // namespace tsugumi::remote
