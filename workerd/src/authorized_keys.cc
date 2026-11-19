#include "tsugumi/workerd/authorized_keys.hh"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <sstream>

#include "tsugumi/rpc/peer_tls.hh"
namespace tsugumi::workerd {
auto Authorized(const std::filesystem::path& file, std::string_view key)
    -> bool {
  // Re-read at every handshake so removing a line revokes new connections.
  const int fd =
      ::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  struct Close {
    int fd;
    ~Close() {
      if (fd >= 0) ::close(fd);
    }
  } close{fd};
  struct stat st{};
  if (fd < 0 || ::fstat(fd, &st) || !S_ISREG(st.st_mode) ||
      st.st_uid != ::geteuid() || (st.st_mode & 0022) || st.st_size > (1 << 20))
    return false;
  std::string data;
  char buffer[4096];
  while (true) {
    const auto n = ::read(fd, buffer, sizeof buffer);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) return false;
    if (!n) break;
    data.append(buffer, static_cast<size_t>(n));
    if (data.size() > (1 << 20)) return false;
  }
  std::istringstream lines(data);
  std::string line;
  bool found = false;
  while (std::getline(lines, line)) {
    line = line.substr(0, line.find('#'));
    std::istringstream fields(line);
    std::string public_key, extra;
    if (!(fields >> public_key)) continue;
    // A malformed configuration fails closed, including unrelated invalid keys.
    if (fields >> extra) return false;
    try {
      peer_tls::ValidateKey(public_key);
    } catch (...) {
      return false;
    }
    found = found || public_key == key;
  }
  return found;
}
}  // namespace tsugumi::workerd
