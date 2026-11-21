#pragma once
// The length-prefixed frame of every Tsugumi byte stream that is not a
// WebSocket: a 4-byte little-endian length, then that many bytes (one
// FlatBuffer). A frame is never empty. Each link states its own maximum, and a
// reader rejects a larger length before allocating anything for it; the
// caller then closes the connection. Reads and writes are blocking and whole:
// no partial frames, retries or timeouts.
//
// Header-only and free of other Tsugumi code, so the standalone Card worker
// (built from the SDK's copy of its sources) can include it.

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tsugumi::rpc::frame {
inline constexpr size_t kPrefixBytes = 4;

struct Error : std::runtime_error {
  enum class Kind {
    Size,       // zero, or over the link's maximum
    Truncated,  // the stream ended inside a frame
    IO,         // read or write failed (errno at the time)
  };
  Kind kind;
  Error(Kind kind, const std::string& message)
      : std::runtime_error(message), kind(kind) {}
};

inline auto EncodePrefix(uint32_t size) -> std::array<char, kPrefixBytes> {
  return {static_cast<char>(size), static_cast<char>(size >> 8),
          static_cast<char>(size >> 16), static_cast<char>(size >> 24)};
}

// Throws Error(Size) unless 0 < size <= max.
inline auto CheckSize(size_t size, size_t max) -> void {
  if (size == 0) throw Error(Error::Kind::Size, "empty frame");
  if (size > max || size > UINT32_MAX)
    throw Error(Error::Kind::Size, "frame of " + std::to_string(size) +
                                       " bytes exceeds the limit of " +
                                       std::to_string(max));
}

// The payload length a prefix announces, checked against max.
inline auto DecodePrefix(const void* prefix, size_t max) -> uint32_t {
  const auto* bytes = static_cast<const uint8_t*>(prefix);
  const uint32_t size = uint32_t{bytes[0]} | uint32_t{bytes[1]} << 8 |
                        uint32_t{bytes[2]} << 16 | uint32_t{bytes[3]} << 24;
  CheckSize(size, max);
  return size;
}

// Reads up to size bytes; fewer only at the end of the stream.
inline auto ReadExact(int fd, char* bytes, size_t size) -> size_t {
  size_t offset = 0;
  while (offset < size) {
    const auto count = ::read(fd, bytes + offset, size - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0)
      throw Error(Error::Kind::IO,
                  std::string("frame read failed: ") + std::strerror(errno));
    if (count == 0) break;
    offset += static_cast<size_t>(count);
  }
  return offset;
}

inline auto WriteAll(int fd, const char* bytes, size_t size) -> void {
#ifdef MSG_NOSIGNAL
  constexpr int flags = MSG_NOSIGNAL;
#else
  constexpr int flags = 0;  // SO_NOSIGPIPE or SIGPIPE ignored on macOS
#endif
  bool socket = true;
  for (size_t offset = 0; offset < size;) {
    auto count = socket ? ::send(fd, bytes + offset, size - offset, flags)
                        : ::write(fd, bytes + offset, size - offset);
    if (count < 0 && socket && errno == ENOTSOCK) {
      socket = false;
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0)
      throw Error(Error::Kind::IO, std::string("frame write failed: ") +
                                       std::strerror(count < 0 ? errno : EPIPE));
    offset += static_cast<size_t>(count);
  }
}

// One whole frame, or nullopt when the stream ends before a frame begins.
inline auto Read(int fd, size_t max) -> std::optional<std::string> {
  std::array<char, kPrefixBytes> prefix{};
  const auto got = ReadExact(fd, prefix.data(), prefix.size());
  if (got == 0) return std::nullopt;
  if (got < prefix.size())
    throw Error(Error::Kind::Truncated, "truncated frame prefix");
  std::string bytes(DecodePrefix(prefix.data(), max), '\0');
  if (ReadExact(fd, bytes.data(), bytes.size()) < bytes.size())
    throw Error(Error::Kind::Truncated, "truncated frame");
  return bytes;
}

inline auto Write(int fd, std::string_view payload, size_t max) -> void {
  CheckSize(payload.size(), max);
  const auto prefix = EncodePrefix(static_cast<uint32_t>(payload.size()));
  WriteAll(fd, prefix.data(), prefix.size());
  WriteAll(fd, payload.data(), payload.size());
}
}  // namespace tsugumi::rpc::frame
