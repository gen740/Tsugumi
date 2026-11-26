#pragma once

#include <unistd.h>

#include <utility>

namespace tsugumi::host {

class Descriptor {
 public:
  explicit Descriptor(int fd = -1) : fd_(fd) {}
  Descriptor(Descriptor&& other) noexcept
      : fd_(std::exchange(other.fd_, -1)) {}
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  ~Descriptor() { reset(); }

  auto get() const -> int { return fd_; }
  auto reset() noexcept -> void {
    if (fd_ >= 0) ::close(std::exchange(fd_, -1));
  }

 private:
  int fd_;
};

}  // namespace tsugumi::host
