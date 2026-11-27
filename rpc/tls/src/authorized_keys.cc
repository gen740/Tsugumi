#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <sstream>
#include <stdexcept>

#include "tsugumi/rpc/peer_tls.hh"

namespace tsugumi::peer_tls {

auto ReadAuthorizedKeys(const std::filesystem::path& file)
    -> AuthorizedKeyFile {
  constexpr auto invalid =
      "authorized_keys must be an owned regular file, "
      "at most 1 MiB and writable only by its owner";
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
      st.st_uid != ::geteuid() || (st.st_mode & 0022) || st.st_size < 0 ||
      static_cast<size_t>(st.st_size) > kAuthorizedKeysLimit)
    throw std::runtime_error(invalid);

  AuthorizedKeyFile file_data;
  auto& data = file_data.text;
  char buffer[4096];
  while (true) {
    const auto n = ::read(fd, buffer, sizeof buffer);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) throw std::runtime_error("Cannot read authorized_keys");
    if (!n) break;
    data.append(buffer, static_cast<size_t>(n));
    if (data.size() > kAuthorizedKeysLimit) throw std::runtime_error(invalid);
  }

  auto remaining = std::string_view(data);
  while (!remaining.empty()) {
    const auto newline = remaining.find('\n');
    const auto line = remaining.substr(
        0, newline == remaining.npos ? remaining.size() : newline + 1);
    remaining.remove_prefix(line.size());
    std::istringstream fields(std::string(line.substr(0, line.find('#'))));
    std::string key, extra;
    if (fields >> key) {
      if (fields >> extra)
        throw std::runtime_error("Malformed authorized_keys");
      ValidateKey(key);
      const auto start = static_cast<size_t>(line.data() - data.data());
      file_data.lines.push_back({std::move(key), start, start + line.size()});
    }
  }
  return file_data;
}

auto Authorized(const std::filesystem::path& file, std::string_view key)
    -> bool {
  // A malformed configuration fails closed, including unrelated invalid keys.
  // Re-read at every handshake so removing a line revokes new connections.
  try {
    return std::ranges::any_of(
        ReadAuthorizedKeys(file).lines,
        [&](const auto& line) { return line.key == key; });
  } catch (...) {
    return false;
  }
}

}  // namespace tsugumi::peer_tls
