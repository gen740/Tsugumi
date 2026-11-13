#include "run_scratch.hh"

#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/magic.h>
#include <sys/vfs.h>
#endif

#include <stdexcept>

#include "util/crypto.hh"

namespace tsugumi {

auto private_disk_scratch(const std::filesystem::path& directory) -> std::filesystem::path {
  const auto path = std::filesystem::absolute(directory).lexically_normal();
  // Walk from the filesystem root so a link in a parent cannot redirect us.
  std::filesystem::path part;
  for (const auto& component : path) {
    part /= component;
    struct stat st{};
    if (::lstat(part.c_str(), &st) != 0) {
      if (::mkdir(part.c_str(), 0700) != 0 || ::lstat(part.c_str(), &st) != 0)
        throw std::runtime_error("cannot create scratch directory " + part.string());
    }
    if (!S_ISDIR(st.st_mode)) throw std::runtime_error("scratch directory is not a folder: " + part.string());
    if ((st.st_uid != 0 && st.st_uid != ::geteuid()) ||
        ((st.st_mode & 0022) != 0 && (st.st_mode & S_ISVTX) == 0))
      throw std::runtime_error("scratch parent is not private: " + part.string());
    if (part == path && (st.st_uid != ::geteuid() || ::chmod(part.c_str(), 0700) != 0))
      throw std::runtime_error("scratch directory is not owned by this user: " + part.string());
  }
#ifdef __linux__
  struct statfs disk{};
  if (::statfs(path.c_str(), &disk) != 0 || disk.f_type == TMPFS_MAGIC || disk.f_type == RAMFS_MAGIC)
    throw std::runtime_error("run scratch must be on disk, not tmpfs: " + path.string());
#endif
  return path;
}

auto default_run_scratch(const std::filesystem::path& project) -> std::filesystem::path {
#ifdef __APPLE__
  const std::filesystem::path temporary = "/private/var/tmp";
#else
  const std::filesystem::path temporary = "/var/tmp";
#endif
  auto user = private_disk_scratch(temporary / ("tsugumi-scratch-" + std::to_string(::geteuid())));
  return user / crypto::sha256_hex(project.string()).substr(0, 16);
}

}  // namespace tsugumi
