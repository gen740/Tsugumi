#include "tsugumi/workerd/protocol.hh"

#include <algorithm>
#include <set>

#include "tsugumi/rpc/frame.hh"
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
  const auto bytes = rpc::frame::EncodePrefix(length);
  return {bytes.data(), bytes.size()};
}
auto frame_length(std::string_view prefix) -> uint32_t {
  if (prefix.size() != rpc::frame::kPrefixBytes)
    throw Error(R::ErrorCode::Invalid, "truncated frame prefix");
  try {
    return rpc::frame::DecodePrefix(prefix.data(), kMaxFrame);
  } catch (const rpc::frame::Error& error) {
    throw Error(R::ErrorCode::Invalid, error.what());
  }
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
auto validate_secret_names(const R::JobSpecT& spec) -> void {
  if (spec.secrets.size() > 256)
    throw Error(R::ErrorCode::Invalid, "too many Card secret names");
  std::set<std::string_view> seen;
  for (const auto& name : spec.secrets) {
    const bool valid =
        !name.empty() && name.size() <= 64 && name[0] >= 'a' &&
        name[0] <= 'z' && std::ranges::all_of(name, [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        });
    if (!valid || !seen.insert(name).second)
      throw Error(R::ErrorCode::Invalid, "invalid Card secret name");
  }
}
auto clear_secret(std::string& value) -> void {
  volatile char* bytes = value.data();
  for (size_t i = 0; i < value.size(); ++i) bytes[i] = 0;
  value.clear();
  value.shrink_to_fit();
}
auto terminal(R::JobState state) -> bool {
  return state == R::JobState::Succeeded || state == R::JobState::Failed ||
         state == R::JobState::Cancelled || state == R::JobState::Interrupted ||
         state == R::JobState::Released;
}
}  // namespace tsugumi::workerd
