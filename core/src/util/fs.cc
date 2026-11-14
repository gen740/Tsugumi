#include "util/fs.hh"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "util/crypto.hh"
#include "util/sparse.hh"
#include "util/text.hh"

namespace tsugumi::fs {

namespace {

void write_all(int fd, const char* p, size_t n, const stdfs::path& what) {
  while (n > 0) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      throw Error("write " + what.string() + ": " + std::strerror(errno));
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
}

}  // namespace

bool valid_relative_path(std::string_view p, bool allow_root) {
  if (p.empty()) return false;
  if (p == ".") return allow_root;
  if (!text::valid_utf8(p)) return false;
  if (p.find('\0') != std::string_view::npos || p.find('\\') != std::string_view::npos)
    return false;
  if (p.front() == '/' || p.back() == '/') return false;
  size_t start = 0;
  while (start <= p.size()) {
    size_t end = p.find('/', start);
    if (end == std::string_view::npos) end = p.size();
    std::string_view seg = p.substr(start, end - start);
    if (seg.empty() || seg == "." || seg == "..") return false;
    start = end + 1;
    if (end == p.size()) break;
  }
  return true;
}

std::optional<std::string> read_range(const stdfs::path& p, uint64_t offset, uint64_t size) {
  int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT || errno == ENOTDIR) return std::nullopt;
    throw Error("open " + p.string() + ": " + std::strerror(errno));
  }
  std::string out(size, '\0');
  size_t got = 0;
  while (got < size) {
    ssize_t r = ::pread(fd, out.data() + got, size - got, static_cast<off_t>(offset + got));
    if (r < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      ::close(fd);
      throw Error("read " + p.string() + ": " + std::strerror(e));
    }
    if (r == 0) break;
    got += static_cast<size_t>(r);
  }
  ::close(fd);
  out.resize(got);
  return out;
}

std::string sha256_file(const stdfs::path& p) {
  int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw Error("open " + p.string() + ": " + std::strerror(errno));
  crypto::Sha256 h;
  std::string buf(1u << 20, '\0');
  while (true) {
    ssize_t r = ::read(fd, buf.data(), buf.size());
    if (r < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      ::close(fd);
      throw Error("read " + p.string() + ": " + std::strerror(e));
    }
    if (r == 0) break;
    h.update(buf.data(), static_cast<size_t>(r));
  }
  ::close(fd);
  return h.finish_hex();
}

std::optional<std::string> read_file(const stdfs::path& p) {
  int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT || errno == ENOTDIR) return std::nullopt;
    throw Error("open " + p.string() + ": " + std::strerror(errno));
  }
  std::string out;
  char buf[65536];
  while (true) {
    ssize_t r = ::read(fd, buf, sizeof buf);
    if (r < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      ::close(fd);
      throw Error("read " + p.string() + ": " + std::strerror(e));
    }
    if (r == 0) break;
    out.append(buf, static_cast<size_t>(r));
  }
  ::close(fd);
  return out;
}

std::string read_file_or_throw(const stdfs::path& p) {
  auto r = read_file(p);
  if (!r) throw Error("missing file " + p.string());
  return *r;
}

void fsync_dir(const stdfs::path& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw Error("open dir " + dir.string() + ": " + std::strerror(errno));
  ::fsync(fd);
  ::close(fd);
}

namespace {

template <class Write>
auto write_atomic_using(const stdfs::path& p, unsigned mode, Write write) -> void {
  stdfs::create_directories(p.parent_path());
  stdfs::path tmp = p;
  tmp += ".tsugumi-tmp-" + crypto::random_token(6);
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
  if (fd < 0) throw Error("create " + tmp.string() + ": " + std::strerror(errno));
  try {
    write(fd, tmp);
    if (::fsync(fd) != 0) throw Error("fsync " + tmp.string() + ": " + std::strerror(errno));
  } catch (...) {
    ::close(fd);
    ::unlink(tmp.c_str());
    throw;
  }
  ::close(fd);
  if (::rename(tmp.c_str(), p.c_str()) != 0) {
    int e = errno;
    ::unlink(tmp.c_str());
    throw Error("rename " + p.string() + ": " + std::strerror(e));
  }
  fsync_dir(p.parent_path());
}

}  // namespace

void write_atomic(const stdfs::path& p, std::string_view data, unsigned mode) {
  write_atomic_using(p, mode, [&](int fd, const stdfs::path& tmp) {
    write_all(fd, data.data(), data.size(), tmp);
  });
}

void remove_file_durable(const stdfs::path& p) {
  if (::unlink(p.c_str()) != 0 && errno != ENOENT)
    throw Error("unlink " + p.string() + ": " + std::strerror(errno));
  fsync_dir(p.parent_path());
}

std::optional<std::string> file_sha256(const stdfs::path& p) {
  auto data = read_file(p);
  if (!data) return std::nullopt;
  return crypto::sha256_hex(*data);
}

bool resolves_inside(const stdfs::path& root, const stdfs::path& p) {
  std::error_code ec;
  auto r = stdfs::weakly_canonical(root, ec);
  if (ec) return false;
  auto c = stdfs::weakly_canonical(p, ec);
  if (ec) return false;
  auto rs = r.string();
  auto cs = c.string();
  return cs == rs || (cs.size() > rs.size() && cs.compare(0, rs.size(), rs) == 0 &&
                      cs[rs.size()] == '/');
}

Listing list_tree(const stdfs::path& root) {
  Listing out;
  std::error_code ec;
  if (!stdfs::is_directory(root, ec)) return out;
  for (auto it = stdfs::recursive_directory_iterator(
           root, stdfs::directory_options::none, ec);
       it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    auto rel = stdfs::relative(it->path(), root).generic_string();
    if (it->is_symlink()) {
      out.links.push_back(rel);
      it.disable_recursion_pending();
    } else if (it->path().filename() == ".tsugumi" && it->is_directory()) {
      it.disable_recursion_pending();  // a project's own state (an external's), never a source
    } else if (it->is_regular_file()) {
      if (rel.find(".tsugumi-tmp-") == std::string::npos) out.files.push_back(rel);
    }
  }
  std::sort(out.files.begin(), out.files.end());
  std::sort(out.links.begin(), out.links.end());
  return out;
}

namespace {

// An fd of `rel` below `root`, walking each component with O_NOFOLLOW.
int open_beneath(const stdfs::path& root, const stdfs::path& rel, int flags) {
  if (rel.is_absolute()) throw Error("file path must be relative to its root");
  int dir = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir < 0) throw Error("open " + root.string() + ": " + std::strerror(errno));
  std::vector<std::string> parts;
  for (const auto& c : rel.lexically_normal())
    if (!c.empty() && c != ".") parts.push_back(c.string());
  for (size_t i = 0; i < parts.size(); ++i) {
    if (parts[i] == "..") {
      ::close(dir);
      throw Error(rel.string() + " leaves " + root.string());
    }
    bool last = i + 1 == parts.size();
    int fd = ::openat(dir, parts[i].c_str(), (last ? flags : (O_RDONLY | O_DIRECTORY)) | O_NOFOLLOW | O_CLOEXEC);
    int err = errno;
    ::close(dir);
    if (fd < 0) throw Error("open " + (root / rel).string() + ": " + std::strerror(err));
    dir = fd;
  }
  return dir;
}

void list_fd(int dir, const std::string& prefix, Listing& out, size_t& remaining) {
  DIR* d = ::fdopendir(dir);
  if (!d) {
    ::close(dir);
    throw Error("list: " + std::string(std::strerror(errno)));
  }
  struct CloseDir {
    DIR* dir;
    ~CloseDir() { ::closedir(dir); }
  } close{d};
  std::vector<std::string> subdirs;
  while (auto* e = ::readdir(d)) {
    std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    if (remaining == 0) throw Error("directory exceeds its transfer entry limit");
    --remaining;
    struct stat st{};
    if (::fstatat(::dirfd(d), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
    if (S_ISLNK(st.st_mode)) out.links.push_back(prefix + name);
    else if (S_ISDIR(st.st_mode)) {
      subdirs.push_back(name);
      out.directories.push_back(prefix + name);
    }
    else if (S_ISREG(st.st_mode)) out.files.push_back(prefix + name);
    else out.special.push_back(prefix + name);
  }
  for (const auto& s : subdirs) {
    int fd = ::openat(::dirfd(d), s.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) out.links.push_back(prefix + s);  // replaced since it was read
    else list_fd(fd, prefix + s + "/", out, remaining);
  }
}

}  // namespace

std::string read_beneath(const stdfs::path& root, const stdfs::path& rel) {
  std::string out;
  stream_beneath(root, rel, UINT64_MAX, [&](std::string_view chunk) { out.append(chunk); });
  return out;
}

auto stream_beneath(const stdfs::path& root, const stdfs::path& rel, uint64_t limit,
                    const std::function<void(std::string_view)>& consume) -> uint64_t {
  const int fd = open_beneath(root, rel, O_RDONLY | O_NONBLOCK);
  struct Close {
    int fd;
    ~Close() { ::close(fd); }
  } close{fd};
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    throw Error((root / rel).string() + " is not a regular file");
  if (st.st_size < 0 || static_cast<uint64_t>(st.st_size) > limit) throw Error("file exceeds its transfer limit");
  uint64_t size = 0;
  char buf[1 << 16];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      int err = errno;
      throw Error("read " + (root / rel).string() + ": " + std::strerror(err));
    }
    if (n == 0) break;
    if (static_cast<uint64_t>(n) > limit - size) throw Error("file exceeds its transfer limit");
    consume({buf, static_cast<size_t>(n)});
    size += static_cast<uint64_t>(n);
  }
  return size;
}

auto copy_beneath(const stdfs::path& root, const stdfs::path& rel, const stdfs::path& destination,
                  const std::function<void(std::string_view)>& on_chunk) -> uint64_t {
  const int source = open_beneath(root, rel, O_RDONLY | O_NONBLOCK);
  struct Close {
    int fd;
    ~Close() { ::close(fd); }
  } close{source};
  uint64_t size = 0;
  write_atomic_using(destination, 0644, [&](int fd, const stdfs::path& temporary) {
    size = copy_sparse_file(source, fd, root / rel, temporary, on_chunk);
  });
  return size;
}

Listing list_beneath(const stdfs::path& root, const stdfs::path& rel, size_t max_entries) {
  Listing out;
  list_fd(open_beneath(root, rel, O_RDONLY | O_DIRECTORY), "", out, max_entries);
  std::sort(out.files.begin(), out.files.end());
  std::sort(out.links.begin(), out.links.end());
  std::sort(out.directories.begin(), out.directories.end());
  return out;
}

FileLock::~FileLock() {
  if (fd_ >= 0) ::close(fd_);
}

std::optional<FileLock> FileLock::try_acquire(const stdfs::path& p) {
  stdfs::create_directories(p.parent_path());
  int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) throw Error("open lock " + p.string() + ": " + std::strerror(errno));
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    int e = errno;
    ::close(fd);
    if (e == EWOULDBLOCK) return std::nullopt;
    throw Error("flock " + p.string() + ": " + std::strerror(e));
  }
  FileLock l;
  l.fd_ = fd;
  return l;
}

uint64_t directory_size(const stdfs::path& p) {
  uint64_t total = 0;
  std::error_code ec;
  if (!stdfs::exists(p, ec)) return 0;
  for (auto it = stdfs::recursive_directory_iterator(p, ec);
       it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (it->is_regular_file(ec) && !it->is_symlink(ec)) total += it->file_size(ec);
  }
  return total;
}

}  // namespace tsugumi::fs
