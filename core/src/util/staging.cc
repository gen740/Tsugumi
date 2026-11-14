#include "util/staging.hh"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <memory>
#include <string>
#include <system_error>

namespace tsugumi::fs {
namespace {

[[noreturn]] auto failed(const char* operation,
                         const std::filesystem::path& path, int error = errno)
    -> void {
  throw std::system_error(error, std::generic_category(),
                          std::string(operation) + " " + path.string() +
                              " (errno " + std::to_string(error) + ")");
}

auto clear_directory(int source, const std::filesystem::path& path) -> void {
  struct stat status{};
  if (::fstat(source, &status) != 0) failed("fstat", path);
  // Unlinking a read-only file is allowed, but requires write/search permission
  // on its parent. chmod only the pinned directory, never a link's target or a
  // regular file (which might be a hard link outside scratch).
  if ((status.st_mode & S_IRWXU) != S_IRWXU &&
      ::fchmod(source, status.st_mode | S_IRWXU) != 0)
    failed("fchmod", path);

  // A new open description has its own directory offset, unlike dup(2).
  const int scan = ::openat(source, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (scan < 0) failed("openat", path);
  std::unique_ptr<DIR, decltype(&::closedir)> entries(::fdopendir(scan),
                                                      ::closedir);
  if (!entries) {
    const int error = errno;
    ::close(scan);
    failed("fdopendir", path, error);
  }
  for (;;) {
    errno = 0;
    const auto* entry = ::readdir(entries.get());
    if (!entry) {
      if (errno) failed("readdir", path);
      break;
    }
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    const auto child_path = path / name;
    const int child = ::openat(source, name.c_str(),
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int flags = 0;
    if (child >= 0) {
      try {
        clear_directory(child, child_path);
      } catch (...) {
        ::close(child);
        throw;
      }
      ::close(child);
      flags = AT_REMOVEDIR;
    } else if (errno == ENOENT) {
      continue;
    } else if (errno != ENOTDIR && errno != ELOOP) {
      failed("openat", child_path);
    }
    // Remove every non-directory entry, including symlinks and FIFOs. An open
    // file or directory does not prevent unlinking it on macOS or Linux.
    if (::unlinkat(source, name.c_str(), flags) != 0 && errno != ENOENT)
      failed("unlinkat", child_path);
  }
}

}  // namespace

auto clear_staging(const std::filesystem::path& path) -> void {
  const int directory =
      ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (directory < 0) failed("open", path);
  try {
    clear_directory(directory, path);
  } catch (...) {
    ::close(directory);
    throw;
  }
  ::close(directory);
}

}  // namespace tsugumi::fs
