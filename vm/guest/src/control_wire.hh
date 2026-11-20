#pragma once

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include "tsugumi/protocol/vm_guest_generated.h"

namespace tsugumi::vm_guest {
namespace wire = Tsugumi::VMGuest;

// Bound allocations from the peer. The largest payload is the store check's
// 64 KiB diagnostic tail; requests and console lines are smaller.
inline constexpr size_t kMaxFrameSize = 1 << 20;

inline auto ReadExact(int fd, std::span<uint8_t> bytes) -> bool {
  for (size_t offset = 0; offset < bytes.size();) {
    const auto count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error(std::strerror(errno));
    if (count == 0) {
      if (offset == 0) return false;
      throw std::runtime_error("VM stream closed mid-frame");
    }
    offset += static_cast<size_t>(count);
  }
  return true;
}

inline auto ReadFrame(int fd) -> std::vector<uint8_t> {
  std::array<uint8_t, sizeof(uint32_t)> prefix;
  if (!ReadExact(fd, prefix)) return {};
  const auto size = flatbuffers::ReadScalar<uint32_t>(prefix.data());
  if (size == 0 || size > kMaxFrameSize)
    throw std::runtime_error("invalid VM frame size");
  std::vector<uint8_t> bytes(size);
  if (!ReadExact(fd, bytes)) throw std::runtime_error("missing VM frame body");
  return bytes;
}

inline auto ReadMessage(int fd) -> std::unique_ptr<wire::MessageT> {
  const auto bytes = ReadFrame(fd);
  if (bytes.empty()) return nullptr;
  flatbuffers::Verifier verifier(bytes.data(), bytes.size());
  if (!wire::VerifyMessageBuffer(verifier))
    throw std::runtime_error("invalid VM message");
  return wire::UnPackMessage(bytes.data());
}

inline auto WriteAll(int fd, const void* data, size_t size) -> bool {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t offset = 0; offset < size;) {
    const auto count = ::write(fd, bytes + offset, size - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    offset += static_cast<size_t>(count);
  }
  return true;
}

template <class T>
auto Send(int fd, T value, uint64_t request_id = 0) -> bool {
  wire::MessageT message;
  message.request_id = request_id;
  message.payload.Set(std::move(value));
  flatbuffers::FlatBufferBuilder builder;
  wire::FinishMessageBuffer(builder, wire::Message::Pack(builder, &message));
  std::array<uint8_t, sizeof(uint32_t)> prefix;
  flatbuffers::WriteScalar<uint32_t>(prefix.data(), builder.GetSize());
  return WriteAll(fd, prefix.data(), prefix.size()) &&
         WriteAll(fd, builder.GetBufferPointer(), builder.GetSize());
}
}  // namespace tsugumi::vm_guest
