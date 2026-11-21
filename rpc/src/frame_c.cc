#include "tsugumi/rpc/frame.h"

#include <cerrno>
#include <cstdlib>
#include <new>

#include "tsugumi/rpc/frame.hh"

namespace frame = tsugumi::rpc::frame;

namespace {
auto Status(const frame::Error& error) -> tsugumi_frame_status {
  switch (error.kind) {
    case frame::Error::Kind::Size:
      return TSUGUMI_FRAME_SIZE;
    case frame::Error::Kind::Truncated:
      return TSUGUMI_FRAME_TRUNCATED;
    case frame::Error::Kind::IO:
      break;
  }
  return TSUGUMI_FRAME_IO;
}
}  // namespace

extern "C" tsugumi_frame_status tsugumi_frame_read(int fd, uint32_t max_bytes,
                                                   uint8_t** bytes,
                                                   uint32_t* size) {
  *bytes = nullptr;
  *size = 0;
  try {
    char prefix[frame::kPrefixBytes];
    const auto got = frame::ReadExact(fd, prefix, sizeof prefix);
    if (got == 0) return TSUGUMI_FRAME_END;
    if (got < sizeof prefix) return TSUGUMI_FRAME_TRUNCATED;
    const auto length = frame::DecodePrefix(prefix, max_bytes);
    auto* buffer = static_cast<uint8_t*>(std::malloc(length));
    if (!buffer) {
      errno = ENOMEM;
      return TSUGUMI_FRAME_IO;
    }
    if (frame::ReadExact(fd, reinterpret_cast<char*>(buffer), length) <
        length) {
      std::free(buffer);
      return TSUGUMI_FRAME_TRUNCATED;
    }
    *bytes = buffer;
    *size = length;
    return TSUGUMI_FRAME_OK;
  } catch (const frame::Error& error) {
    return Status(error);
  } catch (const std::bad_alloc&) {
    errno = ENOMEM;
    return TSUGUMI_FRAME_IO;
  }
}

extern "C" void tsugumi_frame_free(uint8_t* bytes) { std::free(bytes); }

extern "C" tsugumi_frame_status tsugumi_frame_write(int fd,
                                                    const uint8_t* bytes,
                                                    uint32_t size,
                                                    uint32_t max_bytes) {
  try {
    frame::Write(fd, {reinterpret_cast<const char*>(bytes), size}, max_bytes);
    return TSUGUMI_FRAME_OK;
  } catch (const frame::Error& error) {
    return Status(error);
  } catch (const std::bad_alloc&) {
    errno = ENOMEM;
    return TSUGUMI_FRAME_IO;
  }
}
