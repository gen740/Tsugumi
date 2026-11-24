#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "tsugumi/card_abi.h"

namespace tsugumi::workerd {
// The largest frame between the core and a Card worker (tsugumi/rpc/frame.hh).
inline constexpr uint32_t kMaxWorkerFrameBytes = (16u << 20) + 65536;
inline constexpr size_t kViewBudget = 2 * TSUGUMI_MESSAGE_MAX;
inline constexpr auto message_cost(size_t size) -> size_t {
  // An empty payload still occupies an entry in the queue.
  return size == 0 ? 1 : size;
}
inline auto wipe_secret(std::span<char> value) -> void {
  volatile char* bytes = value.data();
  for (size_t i = 0; i < value.size(); ++i) bytes[i] = 0;
}
inline auto clear_secret(std::string& value) -> void {
  wipe_secret(value);
  value.clear();
}
}  // namespace tsugumi::workerd
