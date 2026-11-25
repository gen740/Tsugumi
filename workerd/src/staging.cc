#include "tsugumi/workerd/staging.hh"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/clonefile.h>
#else
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#include <algorithm>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "tsugumi/util/sparse.hh"

namespace tsugumi::fs {
namespace {

[[noreturn]] auto failed(const char* operation,
                         const std::filesystem::path& path, int error = errno)
    -> void {
  throw std::system_error(error, std::generic_category(),
                          std::string(operation) + " " + path.string() +
                              " (errno " + std::to_string(error) + ")");
}

// Pin Linux directories without requiring read permission. Darwin has no
// equivalent: its caller supplies permission recovery with Card authority.
auto pin_directory(int parent, const char* name,
                   const std::function<void(int, const char*)>& recover)
    -> int {
#ifdef __APPLE__
  constexpr int access = O_RDONLY;
#else
  constexpr int access = O_PATH;
  (void)recover;
#endif
  constexpr int flags = access | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
  int fd = ::openat(parent, name, flags);
#ifdef __APPLE__
  if (fd < 0 && errno == EACCES && recover) {
    // Recovery must have no more authority than the Card that wrote staging.
    recover(parent, name);
    fd = ::openat(parent, name, flags);
  }
#endif
  return fd;
}

auto clear_directory(int source, const std::filesystem::path& path,
                     const std::function<void(int, const char*)>& recover,
                     std::string_view preserved_entry = {}) -> void {
  struct stat status{};
  if (::fstat(source, &status) != 0) failed("fstat", path);
  // Unlinking a read-only file is allowed, but requires write/search permission
  // on its parent. chmod only the pinned directory, never a link's target or a
  // regular file (which might be a hard link outside scratch).
  if ((status.st_mode & S_IRWXU) != S_IRWXU) {
#ifdef __APPLE__
    const int changed = ::fchmod(source, status.st_mode | S_IRWXU);
#else
    // fchmod rejects O_PATH descriptors. procfs resolves this pinned inode,
    // including after a rename, without traversing a Card-controlled path.
    const auto pinned = "/proc/self/fd/" + std::to_string(source);
    const int changed = ::chmod(pinned.c_str(), status.st_mode | S_IRWXU);
#endif
    if (changed != 0) failed("chmod pinned directory", path);
  }

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
    if (name == "." || name == ".." || name == preserved_entry) continue;
    const auto child_path = path / name;
    const int child = pin_directory(source, name.c_str(), recover);
    int flags = 0;
    if (child >= 0) {
      try {
        clear_directory(child, child_path, recover);
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

// Open source entries without following links.
class Handle {
 public:
  explicit Handle(int fd) : fd_(fd) {}
  ~Handle() {
    if (fd_ >= 0) ::close(fd_);
  }
  Handle(Handle&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  Handle(const Handle&) = delete;
  auto get() const -> int { return fd_; }

 private:
  int fd_;
};

auto entry_kind(mode_t mode) -> std::string {
  if (S_ISLNK(mode)) return "symbolic link";
  if (S_ISFIFO(mode)) return "FIFO";
  if (S_ISSOCK(mode)) return "socket";
  if (S_ISCHR(mode)) return "character device";
  if (S_ISBLK(mode)) return "block device";
  if (S_ISDIR(mode)) return "directory";
  if (S_ISREG(mode)) return "regular file";
  return "unknown entry";
}

[[noreturn]] auto reject(const std::filesystem::path& path,
                         const std::string& kind, const std::string& reason)
    -> void {
  throw std::runtime_error("worker output " + path.string() + " (" + kind +
                           "): " + reason);
}

auto open_entry(int parent, const std::string& name,
                const std::filesystem::path& path) -> Handle {
  struct stat entry{};
  if (::fstatat(parent, name.c_str(), &entry, AT_SYMLINK_NOFOLLOW) == 0 &&
      !S_ISREG(entry.st_mode) && !S_ISDIR(entry.st_mode))
    reject(path, entry_kind(entry.st_mode),
           "only regular files and directories are allowed");
  const int fd = ::openat(parent, name.c_str(),
                          O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    const int error = errno;
    struct stat st{};
    const auto kind =
        ::fstatat(parent, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0
            ? entry_kind(st.st_mode)
            : "missing or unreadable entry";
    reject(path, kind, "openat: " + std::generic_category().message(error));
  }
  return Handle(fd);
}

auto status_of(int fd, const std::filesystem::path& path) -> struct stat {
  struct stat st{};
  if (::fstat(fd, &st) != 0) failed("fstat", path);
  return st;
}

// Give the destination a new inode with the opened source's contents. Clone
// the opened inode, never a path the Card could replace.
auto clone_file(int source, const std::filesystem::path& label,
                const std::filesystem::path& destination) -> void {
#ifdef __APPLE__
  if (::fclonefileat(source, AT_FDCWD, destination.c_str(), 0) == 0) return;
  const int error = errno;
  if (error == ENOTSUP || error == EXDEV)
    throw std::runtime_error(
        "cannot seal worker output " + label.string() +
        ": its filesystem cannot clone files (clonefile: " +
        std::generic_category().message(error) +
        "); Tsugumi on macOS needs the project on an APFS volume");
  failed("clone artifact member", label, error);
#else
  // The destination is core-owned and is never exposed to the worker.
  const int fd = ::open(destination.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        0600);
  if (fd < 0) failed("open artifact clone", destination);
  Handle written(fd);
  if (::ioctl(fd, FICLONE, source) == 0) return;
  const int error = errno;
  // Filesystems without reflinks (ext4, virtiofs, tmpfs) get a plain copy.
  if (error != EOPNOTSUPP && error != ENOTTY && error != EINVAL &&
      error != EXDEV && error != ENOSYS)
    failed("reflink artifact member", label, error);
  copy_sparse_file(source, fd, label, destination, {});
#endif
}

auto clone_entry(int source, const std::filesystem::path& label,
                 const std::filesystem::path& destination, OutputBudget& budget)
    -> void {
  const auto st = status_of(source, label);
  if (budget.entries == 0)
    reject(label, entry_kind(st.st_mode), "output entry budget exceeded");
  --budget.entries;
  if (S_ISREG(st.st_mode)) {
    const auto bytes = static_cast<uint64_t>(st.st_size);
    if (bytes > budget.bytes)
      reject(label, "regular file", "output byte budget exceeded");
    budget.bytes -= bytes;
    clone_file(source, label, destination);
    return;
  }
  if (!S_ISDIR(st.st_mode))
    reject(label, entry_kind(st.st_mode),
           "only regular files and directories are allowed");
  // The destination is core-owned and is never exposed to the worker.
  if (::mkdir(destination.c_str(), 0700) != 0)
    failed("mkdir artifact clone", destination);
  const int scan =
      ::openat(source, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (scan < 0) failed("openat", label);
  std::unique_ptr<DIR, decltype(&::closedir)> entries(::fdopendir(scan),
                                                      ::closedir);
  if (!entries) {
    const int error = errno;
    ::close(scan);
    failed("fdopendir", label, error);
  }
  for (;;) {
    errno = 0;
    const auto* entry = ::readdir(entries.get());
    if (!entry) {
      if (errno) failed("readdir", label);
      break;
    }
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    auto child = open_entry(source, name, label / name);
    clone_entry(child.get(), label / name, destination / name, budget);
  }
}

}  // namespace

auto clear_staging(const std::filesystem::path& path,
                   const std::function<void(int, const char*)>& recover,
                   std::string_view preserved_entry) -> void {
  const int directory = pin_directory(AT_FDCWD, path.c_str(), recover);
  if (directory < 0) failed("open", path);
  try {
    clear_directory(directory, path, recover, preserved_entry);
  } catch (...) {
    ::close(directory);
    throw;
  }
  ::close(directory);
}

auto clone_staging_artifact(int staging, const std::filesystem::path& name,
                            const std::filesystem::path& destination,
                            OutputBudget budget) -> void {
  if (name.empty() || name.has_parent_path() || name == "." || name == "..")
    reject(name, "artifact name", "expected one directory component");
  const auto source = open_entry(staging, name.string(), name);
  if (!S_ISDIR(status_of(source.get(), name).st_mode))
    reject(name, "artifact", "artifact must be a directory");
  if (std::filesystem::exists(std::filesystem::symlink_status(destination)))
    reject(name, "artifact directory", "clone destination already exists");
  try {
    clone_entry(source.get(), name, destination, budget);
  } catch (...) {
    std::error_code error;
    std::filesystem::remove_all(destination, error);
    throw;
  }
}

auto remove_staging_artifact(int staging, const std::filesystem::path& name)
    -> void {
  if (name.empty() || name.has_parent_path() || name == "." || name == "..")
    reject(name, "artifact name", "expected one directory component");
  const int fd = pin_directory(staging, name.c_str(), {});
  if (fd < 0) {
    if (errno == ENOENT) return;
    failed("openat", name);
  }
  try {
    clear_directory(fd, name, {});
  } catch (...) {
    ::close(fd);
    throw;
  }
  ::close(fd);
  if (::unlinkat(staging, name.c_str(), AT_REMOVEDIR) != 0 && errno != ENOENT)
    failed("unlinkat", name);
}

auto link_artifact(const std::filesystem::path& source,
                   const std::filesystem::path& destination) -> void {
  namespace fs = std::filesystem;
  if (!fs::is_directory(fs::symlink_status(source)))
    throw std::runtime_error("artifact is not a directory");
  fs::create_directory(destination);
  try {
    for (const auto& entry : fs::recursive_directory_iterator(source)) {
      const auto target = destination / entry.path().lexically_relative(source);
      const auto status = entry.symlink_status();
      if (fs::is_directory(status)) {
        fs::create_directory(target);
      } else if (fs::is_regular_file(status)) {
#ifdef __APPLE__
        if (::clonefile(entry.path().c_str(), target.c_str(), CLONE_NOFOLLOW) ==
            0)
          continue;
        if (errno != EXDEV) failed("clone artifact member", entry.path());
#else
        std::error_code error;
        fs::create_hard_link(entry.path(), target, error);
        if (!error) continue;
        if (error != std::errc::cross_device_link)
          throw fs::filesystem_error("link artifact member", entry.path(),
                                     target, error);
#endif
        fs::copy_file(entry.path(), target);
      } else {
        throw std::runtime_error("artifact contains an unsupported entry");
      }
    }
  } catch (...) {
    std::error_code error;
    fs::remove_all(destination, error);
    throw;
  }
}

auto move_staging_artifact(int staging, const std::filesystem::path& name,
                           const std::filesystem::path& destination,
                           OutputBudget& budget) -> void {
  namespace fs = std::filesystem;
  if (name.empty() || name.has_parent_path() || name == "." || name == "..")
    reject(name, "artifact name", "expected one directory component");
  const auto root = open_entry(staging, name.string(), name);
  const auto root_mode = status_of(root.get(), name).st_mode;
  if (!S_ISDIR(root_mode))
    reject(name, "artifact", "artifact must be a directory");
  const auto visit = [&](auto&& self, int fd, const fs::path& path,
                         bool seal) -> void {
    const auto st = status_of(fd, path);
    if (!seal) {
      if (!budget.entries)
        reject(path, entry_kind(st.st_mode), "output entry budget exceeded");
      --budget.entries;
    }
    if (S_ISREG(st.st_mode)) {
      if (!seal) {
        if (st.st_nlink != 1)
          reject(path, "regular file", "hard links in outputs are forbidden");
        const auto bytes = static_cast<uint64_t>(st.st_size);
        if (bytes > budget.bytes)
          reject(path, "regular file", "output byte budget exceeded");
        budget.bytes -= bytes;
      } else if (::fchmod(fd, 0400) != 0) {
        failed("seal artifact file", path);
      }
      return;
    }
    if (!S_ISDIR(st.st_mode))
      reject(path, entry_kind(st.st_mode),
             "only regular files and directories are allowed");
    const int scan = ::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
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
      const std::string member = entry->d_name;
      if (member == "." || member == "..") continue;
      const auto child = open_entry(fd, member, path / member);
      self(self, child.get(), path / member, seal);
    }
    if (seal && ::fchmod(fd, 0500) != 0)
      failed("seal artifact directory", path);
  };
  visit(visit, root.get(), name, false);
  fs::create_directories(destination.parent_path());
  if (fs::exists(fs::symlink_status(destination)))
    reject(name, "artifact directory", "destination already exists");
#ifdef __APPLE__
  // Copying a sealed input can preserve its read-only directory mode.
  // Darwin needs write permission on the directory when changing its parent.
  if (!(root_mode & S_IWUSR) && ::fchmod(root.get(), S_IRWXU) != 0)
    failed("prepare artifact directory for move", name);
#endif
  if (::renameat(staging, name.c_str(), AT_FDCWD, destination.c_str()) != 0)
    failed("move artifact into store", destination);
  try {
    visit(visit, root.get(), destination, true);
  } catch (...) {
    if (::renameat(AT_FDCWD, destination.c_str(), staging, name.c_str()) != 0)
      failed("roll back artifact move", destination);
    throw;
  }
}

}  // namespace tsugumi::fs
