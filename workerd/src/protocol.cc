#include "tsugumi/workerd/protocol.hh"

#include <algorithm>
#include <set>

#include "tsugumi/workerd/frame.hh"

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
  if (bytes.size() > kMaxFrame || !R::VerifyMessageBuffer(v))
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
  flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(bytes.data()),
                          bytes.size());
  if (bytes.size() > kMaxSpec || !v.VerifyBuffer<R::JobSpec>(nullptr))
    throw Error(R::ErrorCode::Rejected, "invalid job specification FlatBuffer");
  auto spec = std::unique_ptr<R::JobSpecT>(
      flatbuffers::GetRoot<R::JobSpec>(bytes.data())->UnPack());
  validate_spec(*spec);
  return spec;
}
auto validate_spec(const R::JobSpecT& s) -> void {
  const auto digest = [](const std::string& value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char c) {
             return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
  };
  const auto store_path = [](const std::string& p) {
    constexpr std::string_view base = "/nix/store/";
    if (!p.starts_with(base) || p.size() <= base.size() + 33 ||
        p.size() > base.size() + 255 || p[base.size() + 32] != '-')
      return false;
    return std::ranges::all_of(
               std::string_view(p).substr(base.size(), 32),
               [](char c) {
                 return std::string_view("0123456789abcdfghijklmnpqrsvwxyz")
                            .find(c) != std::string_view::npos;
               }) &&
           std::ranges::all_of(
               std::string_view(p).substr(base.size() + 33), [](char c) {
                 return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        std::string_view("+-._?=").find(c) !=
                            std::string_view::npos;
               });
  };
  std::set<std::string> unique(s.input_blobs.begin(), s.input_blobs.end());
  if ((s.system != "aarch64-linux" && s.system != "x86_64-linux") ||
      !store_path(s.derivation) || !s.derivation.ends_with(".drv") ||
      !store_path(s.output) || !digest(s.invocation_sha256) ||
      s.input_blobs.empty() || s.input_blobs.size() > 1024 ||
      unique.size() != s.input_blobs.size() ||
      !unique.contains(s.invocation_sha256) ||
      !std::ranges::all_of(s.input_blobs, digest) || !s.timeout_seconds ||
      s.timeout_seconds > 7 * 24 * 3600 ||
      (s.cpu_millis == 0) != (s.memory_bytes == 0) || s.cpu_millis > 256000 ||
      s.memory_bytes > (uint64_t{1} << 40))
    throw Error(R::ErrorCode::Rejected,
                "invalid or out-of-bounds job specification");
}
auto terminal(R::JobState state) -> bool {
  return state == R::JobState::Succeeded || state == R::JobState::Failed ||
         state == R::JobState::Cancelled || state == R::JobState::Interrupted ||
         state == R::JobState::Released;
}
}  // namespace tsugumi::workerd
