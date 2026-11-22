#pragma once

#include <cstdint>

namespace tsugumi {

inline constexpr uint64_t kInvocationLogLimitBytes = uint64_t{128} << 20;
inline constexpr uint64_t kMaxRecordedLogBytes = uint64_t{1} << 30;
// Remote scratch transports escaped JSONL and is discarded on job release.
inline constexpr uint64_t kRemoteLogTransportBytes = uint64_t{16} << 30;

}  // namespace tsugumi
