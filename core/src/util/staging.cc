#include "util/staging.hh"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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

// Every source component is opened without following links. Relative links are
// expanded in userspace against a stack of pinned directories rooted at this
// one declared output, never by the kernel against the host namespace.
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

auto check_copy_cancel(const std::atomic<bool>& cancel) -> void {
  if (cancel.load()) throw std::runtime_error("output transfer cancelled");
}

// Return a pinned target and its physical path relative to the declared root.
// Keeping that path matters for a directory alias containing another ../ link.
struct Resolved {
  Handle handle;
  std::filesystem::path path;
};

auto resolve_entry(int root, const std::filesystem::path& relative,
                   const std::filesystem::path& label,
                   const std::atomic<bool>& cancel) -> Resolved {
  std::vector<Handle> parents;
  parents.push_back(open_entry(root, ".", label));
  std::filesystem::path physical;
  std::deque<std::string> pending;
  for (const auto& part : relative) pending.push_back(part.string());
  unsigned links = 0;
  while (!pending.empty()) {
    check_copy_cancel(cancel);
    const auto name = pending.front();
    pending.pop_front();
    if (name.empty() || name == ".") continue;
    if (name == "..") {
      if (parents.size() == 1)
        reject(label, "symbolic link",
               "target escapes the declared Directory output");
      parents.pop_back();
      physical = physical.parent_path();
      continue;
    }
    struct stat st{};
    if (::fstatat(parents.back().get(), name.c_str(), &st,
                  AT_SYMLINK_NOFOLLOW) != 0)
      reject(label, "missing or unreadable entry",
             "fstatat: " + std::generic_category().message(errno));
    if (S_ISLNK(st.st_mode)) {
      if (++links > 40)
        reject(label, "symbolic link", "cyclic or excessive link chain");
      std::vector<char> buffer(65536);
      const auto count = ::readlinkat(parents.back().get(), name.c_str(),
                                      buffer.data(), buffer.size());
      if (count < 0)
        reject(label, "symbolic link",
               "readlinkat: " + std::generic_category().message(errno));
      if (static_cast<size_t>(count) == buffer.size())
        reject(label, "symbolic link", "target is too long");
      const std::filesystem::path target(
          std::string(buffer.data(), static_cast<size_t>(count)));
      if (target.is_absolute())
        reject(label, "symbolic link", "absolute target is not allowed");
      // readlinkat captured one string even if the link was exchanged. Never
      // open the link itself after validation; expand only that captured text.
      for (auto it = target.end(); it != target.begin();)
        pending.push_front((--it)->string());
      continue;
    }
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
      reject(label, entry_kind(st.st_mode),
             "only regular files and directories are allowed");
    auto opened = open_entry(parents.back().get(), name, label);
    // The inode may have changed since fstatat.
    st = status_of(opened.get(), label);
    physical /= name;
    if (pending.empty()) return {std::move(opened), physical};
    if (!S_ISDIR(st.st_mode))
      reject(label, entry_kind(st.st_mode), "expected a directory component");
    parents.push_back(std::move(opened));
  }
  return {std::move(parents.back()), physical};
}

using Inode = std::pair<dev_t, ino_t>;

auto copy_entry(int root, int source, const std::filesystem::path& physical,
                const std::filesystem::path& label,
                const std::filesystem::path& destination,
                const std::atomic<bool>& cancel, std::vector<Inode>& ancestors)
    -> void {
  check_copy_cancel(cancel);
  const auto st = status_of(source, label);
  if (S_ISREG(st.st_mode)) {
    // Overlapping declared outputs can copy a file twice. The destination is
    // core-owned, contains no symlinks, and is never exposed to the worker.
    const int fd =
        ::open(destination.c_str(),
               O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) failed("open destination", destination);
    Handle written(fd);
    std::vector<char> buffer(65536);
    for (;;) {
      check_copy_cancel(cancel);
      const auto count = ::read(source, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) continue;
      if (count < 0)
        reject(label, "regular file",
               "read: " + std::generic_category().message(errno));
      if (!count) break;
      ssize_t offset = 0;
      while (offset < count) {
        const auto n = ::write(fd, buffer.data() + offset,
                               static_cast<size_t>(count - offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) failed("write", destination);
        offset += n;
      }
    }
    return;
  }
  if (!S_ISDIR(st.st_mode))
    reject(label, entry_kind(st.st_mode),
           "only regular files and directories are allowed");
  const Inode inode{st.st_dev, st.st_ino};
  if (ancestors.size() >= 256 ||
      std::find(ancestors.begin(), ancestors.end(), inode) != ancestors.end())
    reject(label, "directory symbolic link",
           "cyclic or excessive directory traversal");
  ancestors.push_back(inode);
  std::filesystem::create_directories(destination);
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
    check_copy_cancel(cancel);
    errno = 0;
    const auto* entry = ::readdir(entries.get());
    if (!entry) {
      if (errno) failed("readdir", label);
      break;
    }
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    auto child = resolve_entry(root, physical / name, label / name, cancel);
    copy_entry(root, child.handle.get(), child.path, label / name,
               destination / name, cancel, ancestors);
  }
  ancestors.pop_back();
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

auto copy_staging_output(int staging, const std::filesystem::path& relative,
                         const std::filesystem::path& destination,
                         bool directory, const std::atomic<bool>& cancel)
    -> void {
  if (relative.empty() || relative.is_absolute())
    reject(relative, "output path", "expected a path relative to staging");
  std::vector<Handle> parents;
  parents.push_back(open_entry(staging, ".", relative));
  for (const auto& part : relative) {
    check_copy_cancel(cancel);
    if (part == "." || part.empty()) continue;
    if (part == "..")
      reject(relative, "output path", "parent components are not allowed");
    parents.push_back(
        open_entry(parents.back().get(), part.string(), relative));
  }
  const int source = parents.back().get();
  const auto st = status_of(source, relative);
  if (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode))
    reject(
        relative, entry_kind(st.st_mode),
        directory ? "expected a Directory output" : "expected a File output");
  std::filesystem::create_directories(destination.parent_path());
  std::vector<Inode> ancestors;
  copy_entry(source, source, {}, relative, destination, cancel, ancestors);
}

}  // namespace tsugumi::fs
