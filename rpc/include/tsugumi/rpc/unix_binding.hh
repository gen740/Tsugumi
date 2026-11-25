#pragma once

#include <sys/types.h>

#include <filesystem>
#include <string>

namespace tsugumi::unix_socket {

// The filesystem entry of a bound listener, which a later listener may replace.
class Binding {
 public:
  Binding() = default;
  Binding(const Binding&) = delete;
  auto operator=(const Binding&) -> Binding& = delete;
  ~Binding();

  auto Bind(int fd, const std::string& path) -> void;
  auto Remove() noexcept -> void;

 private:
  std::filesystem::path path_;
  dev_t device_ = 0;
  ino_t inode_ = 0;
};

}  // namespace tsugumi::unix_socket
