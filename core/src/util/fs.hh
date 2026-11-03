// File system helpers. All writes that the document depends on go through
// write_atomic (temp file + fsync + rename + directory fsync).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tsugumi::fs {

namespace stdfs = std::filesystem;

class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Validates a project relative Path: UTF-8, no NUL, not absolute, no "." or
// ".." segments (except the single "." root), no empty segments, no backslash.
bool valid_relative_path(std::string_view p, bool allow_root = false);

std::optional<std::string> read_file(const stdfs::path& p);
std::string read_file_or_throw(const stdfs::path& p);
// `size` bytes from `offset` (fewer at the end of the file); nullopt when
// the file does not exist.
std::optional<std::string> read_range(const stdfs::path& p, uint64_t offset, uint64_t size);
// SHA-256 (hex) of a file's content, read in parts.
std::string sha256_file(const stdfs::path& p);
void write_atomic(const stdfs::path& p, std::string_view data, unsigned mode = 0644);
void fsync_dir(const stdfs::path& dir);
void remove_file_durable(const stdfs::path& p);
std::optional<std::string> file_sha256(const stdfs::path& p);

// True when `p` resolves (following symlinks) to somewhere inside `root`.
bool resolves_inside(const stdfs::path& root, const stdfs::path& p);

// Recursively lists regular files below root (relative, sorted). Symlinks are
// reported in `links` and never followed.
struct Listing {
  std::vector<std::string> files;
  std::vector<std::string> links;
};
Listing list_tree(const stdfs::path& root);

// Exclusive non blocking advisory lock held for the lifetime of the object.
class FileLock {
 public:
  FileLock() = default;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
  ~FileLock();
  // Returns nullopt when another process holds the lock.
  static std::optional<FileLock> try_acquire(const stdfs::path& p);

 private:
  int fd_ = -1;
};

uint64_t directory_size(const stdfs::path& p);

}  // namespace tsugumi::fs
