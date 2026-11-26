#pragma once

#include <cstdint>

namespace tsugumi {

inline constexpr uint64_t kProbeOutputBytes = 8192;
inline constexpr uint64_t kProbeTransportBytes = 4 * kProbeOutputBytes;
inline constexpr uint64_t kProbeTimeoutSeconds = 5;
inline constexpr uint64_t kProbeBuildTimeoutSeconds = 60;
inline constexpr uint64_t kInvocationLogLimitBytes = uint64_t{128} << 20;
inline constexpr uint64_t kMaxRecordedLogBytes = uint64_t{1} << 30;
// Remote scratch transports FlatBuffer log entries and is discarded on release.
inline constexpr uint64_t kRemoteLogTransportBytes = uint64_t{16} << 30;

}  // namespace tsugumi
