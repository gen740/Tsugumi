// File system helpers. All writes that the document depends on go through
// write_atomic (temp file + fsync + rename + directory fsync).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
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

class NotRegularFile : public Error {
 public:
  NotRegularFile(const stdfs::path& path, bool folder)
      : Error(path.string() +
              (folder ? " is a folder" : " is not a regular file")),
        folder(folder) {}
  bool folder;
};

// Validates a project relative Path: UTF-8, no NUL, not absolute, no "." or
// ".." segments (except the single "." root), no empty segments, no backslash.
bool valid_relative_path(std::string_view p, bool allow_root = false);

std::optional<std::string> read_file(const stdfs::path& p);
std::string read_file_or_throw(const stdfs::path& p);
// `size` bytes from `offset` (fewer at the end of the file); nullopt when
// the file does not exist.
std::optional<std::string> read_range(const stdfs::path& p, uint64_t offset,
                                      uint64_t size);
// A page of a regular file whose resolved location is inside `root` (also
// resolved). The file is opened once and its size and bytes come from that
// descriptor, so a symlink that leads out of root, or a file replaced after
// the check, is never read. Bytes are read only when `offset` <= size.
struct ContainedRange {
  uint64_t size = 0;
  std::string bytes;
};
// nullopt when the file is missing, not a regular file, or outside root.
std::optional<ContainedRange> read_range_inside(const stdfs::path& root,
                                                const stdfs::path& p,
                                                uint64_t offset,
                                                uint64_t limit);
// SHA-256 (hex) of a file's content, read in parts.
std::string sha256_file(const stdfs::path& p);
// SHA-256 (hex) of a directory tree: every directory name and every regular
// file's name and content hash, in sorted order. Links are hashed by name.
std::string sha256_tree(const stdfs::path& p);
void write_atomic(const stdfs::path& p, std::string_view data,
                  unsigned mode = 0644);
void fsync_dir(const stdfs::path& dir);
void remove_file_durable(const stdfs::path& p);

// True when `p` resolves (following symlinks) to somewhere inside `root`.
bool resolves_inside(const stdfs::path& root, const stdfs::path& p);

// Recursively lists regular files below root (relative, sorted). Symlinks are
// reported in `links` and never followed.
struct Listing {
  std::vector<std::string> files;
  std::vector<std::string> links;
  std::vector<std::string> directories;
  std::vector<std::string> special;
};
Listing list_tree(const stdfs::path& root);

// Below a trusted folder `root` (the run folder, tsugumid's state), files a
// Card wrote: no symlink below `root` is followed, so a Card that swaps a
// checked file for a link after the check gets an error, never another file.
// `rel` is relative to root.
std::string read_beneath(const stdfs::path& root, const stdfs::path& rel);
// A complete regular-file transfer: reject EOF before the size observed at
// open, rather than presenting a truncated stream as a successful transfer.
auto stream_beneath(const stdfs::path& root, const stdfs::path& rel,
                    uint64_t limit,
                    const std::function<void(std::string_view)>& consume)
    -> uint64_t;
// Copy a regular file below root into a destination owned by the caller.
auto copy_beneath(const stdfs::path& root, const stdfs::path& rel,
                  const stdfs::path& destination,
                  const std::function<void(std::string_view)>& on_chunk)
    -> uint64_t;
// Links the validated regular inode on Linux. Returns nullopt if linking is
// unavailable, so callers can use the same checked sparse copy path.
auto link_beneath(const stdfs::path& root, const stdfs::path& rel,
                  const stdfs::path& destination) -> std::optional<uint64_t>;
Listing list_beneath(const stdfs::path& root, const stdfs::path& rel,
                     size_t max_entries = SIZE_MAX);
// Whether `rel` below root is a directory (true) or a regular file (false),
// opened without following links. Anything else throws Error.
bool directory_beneath(const stdfs::path& root, const stdfs::path& rel);

// Exclusive advisory lock held for the lifetime of the object.
class FileLock {
 public:
  FileLock() = default;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
  ~FileLock();
  static FileLock acquire(const stdfs::path& p);
  // Returns nullopt when another process holds the lock.
  static std::optional<FileLock> try_acquire(const stdfs::path& p);

 private:
  static std::optional<FileLock> open(const stdfs::path& p, bool wait);
  int fd_ = -1;
};

uint64_t directory_size(const stdfs::path& p);

}  // namespace tsugumi::fs
