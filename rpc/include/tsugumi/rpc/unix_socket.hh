// Unix socket addresses that do not depend on how deep the socket's folder is.
#pragma once

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace tsugumi::unix_socket {

// The longest pathname sun_path holds with its terminating NUL: 103 bytes on
// macOS, 107 on Linux.
inline constexpr size_t kMaxPath = sizeof(sockaddr_un::sun_path) - 1;

namespace detail {

// FNV-1a: stable across processes and languages (VMHostIPC.swift computes the
// same link name).
inline auto LinkName(const std::string& folder) -> std::string {
  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char byte : folder) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  char text[17];
  std::snprintf(text, sizeof text, "%016llx",
                static_cast<unsigned long long>(hash));
  return text;
}

[[noreturn]] inline auto Fail(const std::string& path, const std::string& why)
    -> void {
  throw std::runtime_error(
      "Unix socket path " + path + " is " + std::to_string(path.size()) +
      " bytes; this system allows " + std::to_string(kMaxPath) + ": " + why);
}

// A link to `folder` inside <root>/tsugumi-<uid>, a folder only this user can
// enter. The socket file itself stays in `folder`, so its own permissions
// still decide who can reach it.
inline auto ShortFolder(const std::string& path, const std::string& folder)
    -> std::string {
  const char* selected = std::getenv("TSUGUMI_SOCKET_DIR");
  const std::string root =
      std::string(selected && *selected ? selected : "/tmp") + "/tsugumi-" +
      std::to_string(::geteuid());
  if (::mkdir(root.c_str(), 0700) != 0 && errno != EEXIST)
    Fail(path, "cannot create " + root + ": " + std::strerror(errno));
  struct stat st{};
  if (::lstat(root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != ::geteuid() || (st.st_mode & 0077))
    Fail(path, root + " must be a folder only this user can access");
  const auto link = root + "/" + LinkName(folder);
  if (::symlink(folder.c_str(), link.c_str()) == 0) return link;
  if (errno != EEXIST)
    Fail(path, "cannot create " + link + ": " + std::strerror(errno));
  char target[4096];
  const auto size = ::readlink(link.c_str(), target, sizeof target);
  if (size >= 0 && std::string(target, static_cast<size_t>(size)) == folder)
    return link;
  // A different folder with the same name: replace the link atomically.
  const auto pending = link + "." + std::to_string(::getpid());
  ::unlink(pending.c_str());
  if (::symlink(folder.c_str(), pending.c_str()) != 0 ||
      ::rename(pending.c_str(), link.c_str()) != 0) {
    const auto error = errno;
    ::unlink(pending.c_str());
    Fail(path, "cannot replace " + link + ": " + std::strerror(error));
  }
  return link;
}

}  // namespace detail

// The address of the socket file at `path`, for bind or connect. A path that
// does not fit in sun_path is reached through a short link to its folder
// (TSUGUMI_SOCKET_DIR, else /tmp, then tsugumi-<uid>/<hash>/<name>). Throws,
// naming the path and the limit, when even that does not fit.
inline auto Address(const std::string& path) -> sockaddr_un {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::string text = path;
  if (text.size() > kMaxPath) {
    const auto slash = path.rfind('/');
    if (path.empty() || path.front() != '/' || slash == std::string::npos)
      detail::Fail(path, "it must be absolute to be shortened");
    text = detail::ShortFolder(path, slash == 0 ? "/" : path.substr(0, slash)) +
           path.substr(slash);
    if (text.size() > kMaxPath)
      detail::Fail(path, "the shortened path " + text + " is still too long");
  }
  std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
  return address;
}

}  // namespace tsugumi::unix_socket
