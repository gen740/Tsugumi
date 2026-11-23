#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "tsugumi/protocol/remote_worker_generated.h"
#include "tsugumi/workerd/frame.hh"

namespace tsugumi::workerd {
namespace R = Tsugumi::RemoteWorker;
inline constexpr uint32_t kMaxFrame = 18 << 20;
inline constexpr uint32_t kChunkBytes = 1 << 20;
inline constexpr uint64_t kMaxBlob = uint64_t{64} << 30;
inline constexpr uint32_t kMaxSpec = 256 << 10;
// The largest live log slice one status reply carries.
inline constexpr uint32_t kLogChunkBytes = 256 << 10;
inline constexpr uint32_t kContract = 15;

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
// Decodes a packed spec. Apply admission policy when accepting a new job.
auto parse_spec(std::string_view bytes) -> std::unique_ptr<R::JobSpecT>;
auto validate_log_limit(const R::JobSpecT& spec) -> void;
// Card secret names follow the definition's rule ([a-z][a-z0-9_]{0,63}), are
// unique and at most 256.
auto validate_secret_names(const R::JobSpecT& spec) -> void;
// Overwrites and releases a buffer that held a Card secret value.
// The job's Card secrets travel from the listener to its invocation through
// this inherited pipe descriptor, as one frame holding a CardSecrets reply.
inline constexpr int kSecretChannelFd = 6;
// The invocation sends live view frames to the listener through this socket.
// Leave room for the log slice and the rest of a Status reply.
inline constexpr int kDisplayChannelFd = 7;
inline constexpr int kViewInputFd = 8;
inline constexpr uint32_t kRemoteDisplayBytes = (16u << 20) + 65536;
// The marker a running job's work directory holds while its invocation waits
// for its Card secrets; it never holds a value.
inline constexpr std::string_view kSecretsWanted = "secrets-wanted";
auto terminal(R::JobState state) -> bool;
}  // namespace tsugumi::workerd
