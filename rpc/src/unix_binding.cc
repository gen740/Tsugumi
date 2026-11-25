#include "tsugumi/rpc/unix_binding.hh"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "tsugumi/rpc/unix_socket.hh"

namespace tsugumi::unix_socket {
namespace {

// Serialize bind and removal in the same directory, including when two
// services are using the same stable socket location during a restart.
struct LockedDirectory {
  int fd;

  explicit LockedDirectory(const std::filesystem::path& path)
      : fd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {
    if (fd < 0) return;
    int result;
    do {
      result = ::flock(fd, LOCK_EX);
    } while (result < 0 && errno == EINTR);
    if (result == 0) return;
    const int error = errno;
    ::close(fd);
    fd = -1;
    errno = error;
  }
  ~LockedDirectory() {
    if (fd >= 0) ::close(fd);
  }
};

}  // namespace

Binding::~Binding() { Remove(); }

auto Binding::Bind(int fd, const std::string& path) -> void {
  auto absolute = std::filesystem::absolute(path);
  const auto parent = absolute.parent_path();
  const auto name = absolute.filename();
  const auto address = Address(path);
  const LockedDirectory directory(parent);
  if (directory.fd < 0)
    throw std::runtime_error("open socket directory: " +
                             std::string(std::strerror(errno)));
  ::unlinkat(directory.fd, name.c_str(), 0);
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) !=
      0)
    throw std::runtime_error("bind unix socket: " +
                             std::string(std::strerror(errno)));
  struct stat file{}, opened{}, current{};
  if (::fstat(directory.fd, &opened) != 0 ||
      ::stat(parent.c_str(), &current) != 0 ||
      opened.st_dev != current.st_dev || opened.st_ino != current.st_ino ||
      ::fstatat(directory.fd, name.c_str(), &file, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISSOCK(file.st_mode))
    throw std::runtime_error("Unix socket directory changed while binding");
  path_ = std::move(absolute);
  device_ = file.st_dev;
  inode_ = file.st_ino;
  if (::fchmodat(directory.fd, name.c_str(), 0600, 0) != 0)
    throw std::runtime_error("chmod unix socket: " +
                             std::string(std::strerror(errno)));
}

auto Binding::Remove() noexcept -> void {
  if (path_.empty()) return;
  try {
    const auto name = path_.filename();
    const LockedDirectory directory(path_.parent_path());
    struct stat current{};
    if (directory.fd >= 0 &&
        ::fstatat(directory.fd, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) ==
            0 &&
        S_ISSOCK(current.st_mode) && current.st_dev == device_ &&
        current.st_ino == inode_)
      ::unlinkat(directory.fd, name.c_str(), 0);
  } catch (...) {
    // Cleanup is best-effort.
  }
  path_.clear();
}

}  // namespace tsugumi::unix_socket
