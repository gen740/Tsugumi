#include "host/state_format.hh"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <string>

#include "util/fs.hh"

namespace tsugumi::host {
StateFormatError::StateFormatError(const std::filesystem::path& directory)
    : std::runtime_error(
          "The host state folder \"" +
          std::filesystem::absolute(directory).lexically_normal().string() +
          "\" was written by another version of Tsugumi. Move or rename this "
          "folder to set it aside, then start Tsugumi again."),
      directory(std::filesystem::absolute(directory).lexically_normal()) {}

auto PrepareStateDirectory(const std::filesystem::path& directory) -> void {
  const auto path = std::filesystem::absolute(directory).lexically_normal();
  std::filesystem::create_directories(path.parent_path());
  // Native and Web clients can start the shared service concurrently. Lock the
  // parent directory while publishing the folder and its marker so another
  // starter cannot mistake a folder being created for unversioned state.
  struct DirectoryLock {
    int fd;
    ~DirectoryLock() {
      if (fd >= 0) ::close(fd);
    }
  } lock{::open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC)};
  if (lock.fd < 0 || ::flock(lock.fd, LOCK_EX) != 0)
    throw std::runtime_error("Cannot lock host state parent directory: " +
                             std::string(std::strerror(errno)));
  const auto marker = path / "format";
  const auto expected = std::to_string(kStateFormat) + "\n";
  if (std::filesystem::create_directory(path)) {
    ::chmod(path.c_str(), 0700);
    fs::write_atomic(marker, expected, 0600);
  } else {
    std::istringstream input(fs::read_file(marker).value_or(""));
    int format = 0;
    if (!(input >> format) || format != kStateFormat ||
        !(input >> std::ws).eof())
      throw StateFormatError(path);
  }
}
}  // namespace tsugumi::host
