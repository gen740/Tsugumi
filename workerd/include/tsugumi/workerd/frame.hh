#pragma once

#include <cstdint>

namespace tsugumi::workerd {
// The largest frame between the core and a Card worker (tsugumi/rpc/frame.hh).
inline constexpr uint32_t kMaxWorkerFrameBytes = 16u << 20;
}  // namespace tsugumi::workerd
