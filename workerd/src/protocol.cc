#include "tsugumi/workerd/protocol.hh"

#include <algorithm>
#include <set>

#include "tsugumi/workerd/frame.hh"
#include "tsugumi/workerd/log_limits.hh"

namespace tsugumi::workerd {
auto encode(R::MessageT message) -> std::string {
  flatbuffers::FlatBufferBuilder b;
  R::FinishMessageBuffer(b, R::Message::Pack(b, &message));
  if (b.GetSize() > kMaxFrame)
    throw Error(R::ErrorCode::Invalid, "frame exceeds 2 MiB");
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}
auto decode(std::string_view bytes) -> std::unique_ptr<R::MessageT> {
  flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(bytes.data()),
                          bytes.size());
  if (!R::VerifyMessageBuffer(v))
    throw Error(R::ErrorCode::Invalid, "invalid remote worker FlatBuffer");
  auto message =
      std::unique_ptr<R::MessageT>(R::GetMessage(bytes.data())->UnPack());
  if (message->payload.type == R::Payload::NONE)
    throw Error(R::ErrorCode::Invalid, "missing remote worker payload");
  return message;
}
auto length_prefix(uint32_t length) -> std::string {
  const auto bytes = encode_length(length);
  return {bytes.data(), bytes.size()};
}
auto frame_length(std::string_view prefix) -> uint32_t {
  if (prefix.size() != 4)
    throw Error(R::ErrorCode::Invalid, "truncated frame prefix");
  const auto n = decode_length(prefix.data());
  if (n == 0 || n > kMaxFrame)
    throw Error(R::ErrorCode::Invalid, "invalid frame length");
  return n;
}
auto failure(R::ErrorCode code, std::string message) -> R::MessageT {
  R::FailureT error;
  error.code = code;
  error.message = message.substr(0, 4096);
  return reply(std::move(error));
}
auto parse_spec(std::string_view bytes) -> std::unique_ptr<R::JobSpecT> {
  return std::unique_ptr<R::JobSpecT>(
      flatbuffers::GetRoot<R::JobSpec>(bytes.data())->UnPack());
}
auto validate_log_limit(const R::JobSpecT& spec) -> void {
  if (spec.log_limit_bytes == 0 ||
      spec.log_limit_bytes > kInvocationLogLimitBytes)
    throw Error(R::ErrorCode::Invalid, "invalid invocation log limit");
}
auto terminal(R::JobState state) -> bool {
  return state == R::JobState::Succeeded || state == R::JobState::Failed ||
         state == R::JobState::Cancelled || state == R::JobState::Interrupted ||
         state == R::JobState::Released;
}
}  // namespace tsugumi::workerd
