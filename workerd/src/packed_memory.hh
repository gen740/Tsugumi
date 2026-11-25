#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "tsugumi/card_abi.h"

namespace tsugumi::workerd {

class PackedDescriptor {
 public:
  explicit PackedDescriptor(int fd = -1) : fd_(fd) {}
  ~PackedDescriptor() {
    if (fd_ >= 0) ::close(fd_);
  }
  PackedDescriptor(const PackedDescriptor&) = delete;
  PackedDescriptor(PackedDescriptor&& other) noexcept
      : fd_(std::exchange(other.fd_, -1)) {}

  auto get() const -> int { return fd_; }

 private:
  int fd_;
};

class PackedMapping {
 public:
  PackedMapping(int fd, int protection) {
    void* data = ::mmap(nullptr, TSUGUMI_MESSAGE_MAX, protection, MAP_SHARED,
                        fd, 0);
    if (data == MAP_FAILED)
      throw std::system_error(errno, std::generic_category(),
                              "map packed Card buffer");
    data_ = static_cast<uint8_t*>(data);
  }
  ~PackedMapping() {
    if (data_) ::munmap(data_, TSUGUMI_MESSAGE_MAX);
  }
  PackedMapping(const PackedMapping&) = delete;
  PackedMapping(PackedMapping&& other) noexcept
      : data_(std::exchange(other.data_, nullptr)) {}

  auto data() const -> const uint8_t* { return data_; }
  auto data() -> uint8_t* { return data_; }

 private:
  uint8_t* data_;
};

inline auto receive_packed_descriptor(int socket) -> PackedDescriptor {
  char marker = 0;
  iovec bytes{&marker, 1};
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
  msghdr message{};
  message.msg_iov = &bytes;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  ssize_t count;
  do {
    count = ::recvmsg(socket, &message, 0);
  } while (count < 0 && errno == EINTR);
  if (count < 0)
    throw std::system_error(errno, std::generic_category(),
                            "receive packed Card descriptor");
  const auto* rights = CMSG_FIRSTHDR(&message);
  int fd = -1;
  if (rights && rights->cmsg_level == SOL_SOCKET &&
      rights->cmsg_type == SCM_RIGHTS && rights->cmsg_len >= CMSG_LEN(sizeof(fd)))
    std::memcpy(&fd, CMSG_DATA(rights), sizeof(fd));
  PackedDescriptor result(fd);
  if (count != 1 || marker != 1 || fd < 0 ||
      rights->cmsg_len != CMSG_LEN(sizeof(fd)) ||
      (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0)
    throw std::runtime_error("invalid packed Card descriptor");
  if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
    throw std::system_error(errno, std::generic_category(),
                            "protect packed Card descriptor");
  return result;
}

}  // namespace tsugumi::workerd
