#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "tsugumi/protocol/remote_worker_generated.h"

namespace tsugumi::workerd {
namespace R = Tsugumi::RemoteWorker;
inline constexpr uint32_t kMaxFrame = 2 << 20;
inline constexpr uint32_t kChunkBytes = 1 << 20;
inline constexpr uint64_t kMaxBlob = uint64_t{64} << 30;
inline constexpr uint32_t kMaxSpec = 256 << 10;
inline constexpr uint32_t kContract = 4;

struct Error : std::runtime_error {
  R::ErrorCode code;
  Error(R::ErrorCode code, const std::string& message)
      : std::runtime_error(message), code(code) {}
};

auto encode(R::MessageT message) -> std::string;
auto decode(std::string_view bytes) -> std::unique_ptr<R::MessageT>;
// A carrier supplies exact reads/writes. No HTTP, TLS or socket assumptions.
auto length_prefix(uint32_t length) -> std::string;
auto frame_length(std::string_view prefix) -> uint32_t;
template <typename T>
auto reply(T value) -> R::MessageT {
  R::ReplyT response;
  response.body.Set(std::move(value));
  R::MessageT message;
  message.payload.Set(std::move(response));
  return message;
}
auto failure(R::ErrorCode code, std::string message) -> R::MessageT;
auto parse_spec(std::string_view bytes) -> std::unique_ptr<R::JobSpecT>;
auto validate_spec(const R::JobSpecT& spec) -> void;
auto terminal(R::JobState state) -> bool;
}  // namespace tsugumi::workerd
