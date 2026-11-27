#include "tsugumi/util/process.hh"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/event.h>
#endif

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace tsugumi::proc {
namespace {

// How a child's writes reach a pipe read depends on the OS and on timing: a
// macOS reader often wakes for one or two lines. Collect complete stderr lines
// across reads and deliver them at most once per interval (or once enough
// bytes have collected), so a consumer that stores each delivery sees a
// number of deliveries bounded by elapsed time and volume, not by reads.
constexpr auto kStderrFlushInterval = std::chrono::milliseconds(50);
constexpr size_t kStderrFlushBytes = 256 << 10;

#ifndef __linux__
std::mutex fork_mutex;
#endif

auto make_pipe(int (&fds)[2]) -> void {
#ifdef __linux__
  if (::pipe2(fds, O_CLOEXEC) != 0)
    throw std::system_error(errno, std::generic_category(), "pipe2");
#else
  // Keep fork in this process helper out of the pipe/fcntl window.
  std::lock_guard lock(fork_mutex);
  if (::pipe(fds) != 0)
    throw std::system_error(errno, std::generic_category(), "pipe");
  if (::fcntl(fds[0], F_SETFD, FD_CLOEXEC) == 0 &&
      ::fcntl(fds[1], F_SETFD, FD_CLOEXEC) == 0)
    return;
  const int error = errno;
  ::close(fds[0]);
  ::close(fds[1]);
  throw std::system_error(error, std::generic_category(), "pipe close-on-exec");
#endif
}

auto child_exit_event(pid_t pid) -> int {
#ifdef __APPLE__
  const int queue = ::kqueue();
  if (queue < 0) return -1;
  struct kevent change;
  EV_SET(&change, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
  if (::fcntl(queue, F_SETFD, FD_CLOEXEC) == 0 &&
      ::kevent(queue, &change, 1, nullptr, 0, nullptr) == 0)
    return queue;
  ::close(queue);
  return -1;
#elif defined(SYS_pidfd_open)
  return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
#else
  return -1;
#endif
}

#ifdef __linux__
// Use the kernel directory records directly: libc directory streams may
// allocate or lock after fork. Enumerating the open descriptors also covers
// descriptors above a subsequently lowered RLIMIT_NOFILE.
template <class Keep>
auto close_open_fds(Keep keep) -> bool {
  struct DirEntry {
    uint64_t inode;
    int64_t offset;
    uint16_t length;
    uint8_t type;
    char name;
  };
  const int dir = ::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir < 0) return false;
  alignas(DirEntry) char buffer[4096];
  while (true) {
    const long count = ::syscall(SYS_getdents64, dir, buffer, sizeof buffer);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      const int error = errno;
      ::close(dir);
      errno = error;
      return count == 0;
    }
    for (long pos = 0; pos < count;) {
      uint16_t length;
      std::memcpy(&length, buffer + pos + offsetof(DirEntry, length),
                  sizeof length);
      const char* name = buffer + pos + offsetof(DirEntry, name);
      pos += length;
      if (*name < '0' || *name > '9') continue;
      int fd = 0;
      for (; *name; ++name) fd = fd * 10 + (*name - '0');
      // Linux releases the descriptor even when close reports EINTR.
      if (fd >= 3 && fd != dir && !keep(fd) &&
          ::close(fd) != 0 && errno != EBADF && errno != EINTR) {
        const int error = errno;
        ::close(dir);
        errno = error;
        return false;
      }
    }
  }
}
#endif

}  // namespace

pid_t spawn(const Spec& spec) {
  if (spec.argv.empty()) throw std::invalid_argument("empty argv");
  std::vector<char*> argv;
  std::vector<char*> envp;
  for (const auto& a : spec.argv) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  for (const auto& e : spec.env) envp.push_back(const_cast<char*>(e.c_str()));
  envp.push_back(nullptr);
  // The child of a multithreaded process allocates nothing: what it needs is
  // made here.
  constexpr size_t kMaxFds = 16;
  if (spec.fds.size() > kMaxFds)
    throw std::invalid_argument("too many fds for a child");
  std::pair<int, int> mapped[kMaxFds];
  size_t nmapped = 0;
  for (auto [child, parent] : spec.fds) {
    if (child < 0 || child == INT_MAX)
      throw std::invalid_argument("invalid child fd");
    mapped[nmapped++] = {child, parent};
  }
#ifndef __linux__
  const long max_fd = ::sysconf(_SC_OPEN_MAX);
  if (max_fd < 0) throw std::runtime_error("cannot determine descriptor limit");
#endif
  int err_pipe[2];
  make_pipe(err_pipe);
#ifndef __linux__
  std::unique_lock fork_lock(fork_mutex);
#endif
  pid_t pid = ::fork();
#ifndef __linux__
  if (pid != 0)
    fork_lock.unlock();
  else
    fork_lock.release();
#endif
  if (pid < 0) {
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    throw std::runtime_error("fork failed");
  }
  struct ChildError {
    int op = 0;
    int err = 0;
  };
  enum ChildOp {
    kOpSetup = 0,
    kOpChdir = 1,
    kOpSetgid = 2,
    kOpSetuid = 3,
    kOpExecve = 4,
    kOpCgroup = 5,
  };
  if (pid == 0) {
    ::close(err_pipe[0]);
    ::setpgid(0, 0);
    // Reset signal dispositions the daemon changed.
    ::signal(SIGPIPE, SIG_DFL);
    ::signal(SIGCHLD, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
    auto fail = [&](int op) {
      const ChildError ce{op, errno ? errno : EIO};
      size_t written = 0;
      while (written < sizeof ce) {
        ssize_t count =
            ::write(err_pipe[1], reinterpret_cast<const char*>(&ce) + written,
                    sizeof ce - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        written += static_cast<size_t>(count);
      }
      ::_exit(127);
    };
    int devnull = ::open("/dev/null", O_RDWR);
    if (devnull < 0) fail(kOpSetup);
    if (::dup2(devnull, 0) < 0) fail(kOpSetup);
    if (::dup2(spec.stdout_fd >= 0 ? spec.stdout_fd : devnull, 1) < 0)
      fail(kOpSetup);
    if (::dup2(spec.stderr_fd >= 0 ? spec.stderr_fd : devnull, 2) < 0)
      fail(kOpSetup);
    int min_temp = 64;
    for (size_t i = 0; i < nmapped; ++i)
      if (mapped[i].first >= min_temp) min_temp = mapped[i].first + 1;
    if (err_pipe[1] < min_temp) {
      int moved = ::fcntl(err_pipe[1], F_DUPFD_CLOEXEC, min_temp);
      if (moved < 0) fail(kOpSetup);
      ::close(err_pipe[1]);
      err_pipe[1] = moved;
    }
    if (err_pipe[1] == INT_MAX) {
      errno = EMFILE;
      fail(kOpSetup);
    }
    if (err_pipe[1] >= min_temp) min_temp = err_pipe[1] + 1;
    // Move mapped fds to high numbers first to avoid clobbering.
    for (size_t i = 0; i < nmapped; ++i) {
      mapped[i].second = ::fcntl(mapped[i].second, F_DUPFD, min_temp);
      if (mapped[i].second < 0) fail(kOpSetup);
    }
    for (size_t i = 0; i < nmapped; ++i) {
      if (::dup2(mapped[i].second, mapped[i].first) < 0) fail(kOpSetup);
      ::close(mapped[i].second);
    }
    // Close everything else, however high (the daemon may hold thousands).
    auto keep = [&](int fd) {
      if (fd == err_pipe[1]) return true;
      for (size_t i = 0; i < nmapped; ++i)
        if (fd == mapped[i].first) return true;
      return false;
    };
#ifdef __linux__
    bool all_closed = false;
#ifdef SYS_close_range
    unsigned preserved[kMaxFds + 1];
    size_t count = 0;
    preserved[count++] = static_cast<unsigned>(err_pipe[1]);
    for (size_t i = 0; i < nmapped; ++i)
      if (mapped[i].first >= 3)
        preserved[count++] = static_cast<unsigned>(mapped[i].first);
    std::sort(preserved, preserved + count);
    unsigned from = 3;
    all_closed = true;
    for (size_t i = 0; i < count && all_closed; ++i) {
      const auto fd = preserved[i];
      if (fd < from) continue;
      if (fd > from)
        all_closed = ::syscall(SYS_close_range, from, fd - 1, 0) == 0;
      from = fd + 1;
    }
    if (all_closed)
      all_closed = ::syscall(SYS_close_range, from, UINT_MAX, 0) == 0;
#endif
    if (!all_closed && !close_open_fds(keep)) fail(kOpSetup);
#else
    for (int fd = 3; fd < max_fd; ++fd)
      if (!keep(fd) && ::close(fd) != 0 && errno != EBADF) fail(kOpSetup);
#endif
    if (!spec.cgroup_procs.empty()) {
      const int fd = ::open(spec.cgroup_procs.c_str(), O_WRONLY | O_CLOEXEC);
      if (fd < 0) fail(kOpCgroup);
      ssize_t count;
      do {
        count = ::write(fd, "0", 1);
      } while (count < 0 && errno == EINTR);
      if (count != 1) fail(kOpCgroup);
      ::close(fd);
    }
    if (!spec.cwd.empty() && ::chdir(spec.cwd.c_str()) != 0) fail(kOpChdir);
    if (spec.gid && ::setgid(*spec.gid) != 0) fail(kOpSetgid);
    if (spec.uid && ::setuid(*spec.uid) != 0) fail(kOpSetuid);
    ::execve(argv[0], argv.data(), envp.data());
    fail(kOpExecve);
  }
  ::close(err_pipe[1]);
  ::setpgid(pid, pid);
  ChildError child_err{};
  size_t got = 0;
  while (got < sizeof child_err) {
    ssize_t n = ::read(err_pipe[0], reinterpret_cast<char*>(&child_err) + got,
                       sizeof child_err - got);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    got += static_cast<size_t>(n);
  }
  ::close(err_pipe[0]);
  if (got == sizeof child_err) {
    const char* op_name = "setup";
    const char* op_path = "";
    switch (child_err.op) {
      case kOpChdir:
        op_name = "chdir";
        op_path = spec.cwd.c_str();
        break;
      case kOpSetgid:
        op_name = "setgid";
        break;
      case kOpSetuid:
        op_name = "setuid";
        break;
      case kOpCgroup:
        op_name = "join Card cgroup";
        op_path = spec.cgroup_procs.c_str();
        break;
      case kOpExecve:
        op_name = "execve";
        op_path = spec.argv[0].c_str();
        break;
      default:
        break;
    }
    const std::string line = std::string(op_name) + " " + op_path + ": " +
                             std::strerror(child_err.err) + "\n";
    const int report_fd = spec.stderr_fd >= 0 ? spec.stderr_fd : STDERR_FILENO;
    size_t written = 0;
    while (written < line.size()) {
      ssize_t w =
          ::write(report_fd, line.data() + written, line.size() - written);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) break;
      written += static_cast<size_t>(w);
    }
    ::waitpid(pid, nullptr, 0);
    throw std::runtime_error("cannot start " + spec.argv[0] + ": " +
                             std::strerror(child_err.err));
  }
  return pid;
}

void kill_group(pid_t pid, int sig) {
  if (pid > 0) ::kill(-pid, sig);
}

std::optional<int> try_wait(pid_t pid) {
  int status = 0;
  pid_t r = ::waitpid(pid, &status, WNOHANG);
  if (r == pid) return status;
  return std::nullopt;
}

int wait(pid_t pid) {
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return status;
}

Result run(const Spec& spec_in, const std::atomic<bool>* cancel,
           std::optional<std::chrono::steady_clock::time_point> deadline,
           const std::function<void(LineChunk)>& on_stderr,
           std::optional<size_t> stderr_line_limit,
           std::chrono::milliseconds grace, LogFraming framing) {
  int out_pipe[2];
  int err_pipe[2];
  Spec spec = spec_in;
  make_pipe(out_pipe);
  try {
    make_pipe(err_pipe);
  } catch (...) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    throw;
  }
  spec.stdout_fd = out_pipe[1];
  spec.stderr_fd = err_pipe[1];
  pid_t pid;
  try {
    pid = spawn(spec);
  } catch (...) {
    for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]})
      ::close(fd);
    throw;
  }
  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  const bool framed = framing == LogFraming::stdout_size_prefixed;
  if (framed) std::swap(out_pipe[0], err_pipe[0]);
  Result res;
  std::exception_ptr log_error;
  const auto deliver = [&](LineChunk chunk) {
    if (chunk.lines.empty() && !chunk.truncated) return;
    try {
      on_stderr(chunk);
    } catch (...) {
      log_error = std::current_exception();
      kill_group(pid, SIGKILL);
    }
  };
  std::string pending;
  std::vector<std::string> lines;
  bool truncated = false;
  size_t line_bytes = 0;
  // Set when `lines` gains its first entry since the last delivery.
  std::optional<std::chrono::steady_clock::time_point> flush_at;
  const auto flush = [&] {
    deliver({lines, truncated});
    lines.clear();
    line_bytes = 0;
    flush_at.reset();
  };
  const auto collect = [&](std::string line) {
    line_bytes += line.size();
    lines.push_back(std::move(line));
    if (!flush_at)
      flush_at = std::chrono::steady_clock::now() + kStderrFlushInterval;
  };
  const auto collect_frames = [&](std::string_view bytes) {
    pending.append(bytes);
    size_t at = 0;
    constexpr size_t header_bytes = sizeof(uint32_t);
    while (pending.size() - at >= header_bytes) {
      const auto* header =
          reinterpret_cast<const unsigned char*>(pending.data() + at);
      uint32_t size = 0;
      for (size_t i = 0; i < header_bytes; ++i)
        size |= static_cast<uint32_t>(header[i]) << (i * 8);
      const size_t total = header_bytes + static_cast<size_t>(size);
      if (stderr_line_limit && total > *stderr_line_limit) {
        truncated = true;
        std::string{}.swap(pending);
        return;
      }
      if (total > pending.size() - at) break;
      collect(pending.substr(at, total));
      at += total;
    }
    pending.erase(0, at);
  };
  bool out_open = true;
  bool err_open = true;
  std::optional<int> exit_event;
  std::optional<std::chrono::steady_clock::time_point> kill_at;
  bool term_sent = false;
  while (true) {
    if (!out_open && !err_open) {
      if (!cancel && !deadline) break;
      // Observe exit without releasing the PID while the process group may
      // still need to be signalled.
      siginfo_t info{};
      if (::waitid(P_PID, static_cast<id_t>(pid), &info,
                   WEXITED | WNOHANG | WNOWAIT) == 0) {
        // Darwin can report a stopped child even with only WEXITED.
        if (info.si_pid == pid &&
            (info.si_code == CLD_EXITED || info.si_code == CLD_KILLED ||
             info.si_code == CLD_DUMPED))
          break;
      } else if (errno == ECHILD) {
        break;
      }
      if (!exit_event) exit_event = child_exit_event(pid);
    }
    pollfd fds[] = {{out_open ? out_pipe[0] : -1, POLLIN, 0},
                    {err_open ? err_pipe[0] : -1, POLLIN, 0},
                    {exit_event.value_or(-1), POLLIN, 0}};
    int timeout_ms = (cancel && !term_sent) || exit_event == -1 ? 100 : -1;
    const auto poll_until = [&](const auto& until) {
      if (!until) return;
      const auto left = std::chrono::ceil<std::chrono::milliseconds>(
          *until - std::chrono::steady_clock::now());
      timeout_ms = static_cast<int>(std::clamp<int64_t>(
          left.count(), 0, timeout_ms < 0 ? INT_MAX : timeout_ms));
    };
    poll_until(flush_at);
    poll_until(term_sent ? kill_at : deadline);
    if (cancel && !term_sent && cancel->load()) timeout_ms = 0;
    ::poll(fds, 3, timeout_ms);
    if (fds[2].revents & (POLLERR | POLLNVAL)) {
      ::close(*exit_event);
      exit_event = -1;
    }
    char buf[8192];
    if (out_open &&
        (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
      ssize_t r = ::read(out_pipe[0], buf, sizeof buf);
      if (r < 0 && errno == EINTR) {
      } else if (r <= 0)
        out_open = false;
      else
        res.out.append(buf, static_cast<size_t>(r));
    }
    if (err_open &&
        (fds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
      ssize_t r = ::read(err_pipe[0], buf, sizeof buf);
      if (r < 0 && errno == EINTR) {
      } else if (r <= 0) {
        err_open = false;
      } else if (on_stderr && !log_error && !truncated) {
        std::string_view remaining(buf, static_cast<size_t>(r));
        if (framed) {
          collect_frames(remaining);
          remaining = {};
        }
        while (!remaining.empty()) {
          const auto newline = remaining.find('\n');
          const auto fragment = remaining.substr(
              0, newline == remaining.npos ? remaining.size() : newline + 1);
          if (stderr_line_limit &&
              fragment.size() > *stderr_line_limit - pending.size()) {
            truncated = true;
            std::string{}.swap(pending);
            break;
          }
          remaining.remove_prefix(fragment.size());
          if (newline == remaining.npos) {
            pending.append(fragment);
            continue;
          }
          if (pending.empty()) {
            collect(std::string(fragment));
          } else {
            pending.append(fragment);
            collect(std::move(pending));
            pending.clear();
          }
        }
        if (truncated || line_bytes >= kStderrFlushBytes) flush();
      }
    }
    auto now = std::chrono::steady_clock::now();
    if (flush_at && now >= *flush_at) flush();
    if (!term_sent) {
      bool stop = false;
      if (cancel && cancel->load()) {
        res.cancelled = true;
        stop = true;
      }
      if (deadline && now >= *deadline) {
        res.timed_out = true;
        stop = true;
      }
      if (stop) {
        kill_group(pid, SIGTERM);
        term_sent = true;
        kill_at = now + grace;
      }
    } else if (kill_at && now >= *kill_at) {
      kill_group(pid, SIGKILL);
      kill_at.reset();
    }
  }
  if (!pending.empty()) {
    if (framed)
      truncated = true;
    else
      collect(std::move(pending));
  }
  if ((!lines.empty() || (framed && !pending.empty())) && !log_error) flush();
  ::close(out_pipe[0]);
  ::close(err_pipe[0]);
  if (exit_event.value_or(-1) >= 0) ::close(*exit_event);
  // Make sure no orphaned grandchildren survive a cancelled build.
  if (term_sent) kill_group(pid, SIGKILL);
  res.status = wait(pid);
  if (log_error) std::rethrow_exception(log_error);
  return res;
}

bool exited_ok(int status) {
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string describe(int status) {
  if (WIFEXITED(status))
    return "exit status " + std::to_string(WEXITSTATUS(status));
  if (WIFSIGNALED(status))
    return std::string("signal ") + std::to_string(WTERMSIG(status));
  return "unknown status";
}

}  // namespace tsugumi::proc
