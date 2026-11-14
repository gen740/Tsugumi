#pragma once

#include <cstdint>
#include <stdexcept>

namespace tsugumi::remote {

inline constexpr uint64_t kMaxLogLimitBytes = uint64_t{16} << 20;
// InvocationLog::finish writes one bounded journal notice outside the budget.
inline constexpr uint64_t kLogNoticeBytes = 128;
inline constexpr uint64_t kMaxLogResultBytes = kMaxLogLimitBytes + kLogNoticeBytes;

inline auto checked_log_limit(uint64_t limit) -> uint64_t {
  if (!limit || limit > kMaxLogLimitBytes)
    throw std::runtime_error("invalid log-limit-bytes: expected 1..16777216");
  return limit;
}

}  // namespace tsugumi::remote
