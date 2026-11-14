#include "run_scratch.hh"

#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/magic.h>
#include <sys/vfs.h>
#endif

#include <algorithm>
#include <cerrno>
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
      // Another project's core may create the shared parent at the same moment.
      if ((::mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) || ::lstat(part.c_str(), &st) != 0)
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

auto measure_run_scratch(const std::filesystem::path& root) -> std::vector<RunScratchUsage> {
  namespace fs = std::filesystem;
  std::vector<RunScratchUsage> result;
  const auto measure = [&](const fs::path& path) {
    uint64_t bytes = 0;
    if (fs::is_directory(fs::symlink_status(path)))
      for (const auto& entry : fs::recursive_directory_iterator(path)) {
        if (fs::is_regular_file(entry.symlink_status())) bytes += entry.file_size();
      }
    result.push_back({path.lexically_relative(root).generic_string(), bytes});
  };
  if (fs::is_directory(fs::symlink_status(root / "runs")))
    for (const auto& entry : fs::directory_iterator(root / "runs")) measure(entry.path());
  if (fs::exists(fs::symlink_status(root / "recovery"))) measure(root / "recovery");
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
  return result;
}

auto remove_run_scratch(const std::filesystem::path& path) -> bool {
  namespace fs = std::filesystem;
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) return true;
  if (error) return false;
  if (!fs::exists(status)) return true;
  if (fs::is_directory(status)) {
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, error);
    if (error) return false;
    std::vector<fs::path> children;
    for (fs::directory_iterator it(path, error), end; !error && it != end; it.increment(error))
      children.push_back(it->path());
    if (error) return false;
    for (const auto& child : children)
      if (!remove_run_scratch(child)) return false;
  }
  fs::remove(path, error);
  return !error;
}

}  // namespace tsugumi
