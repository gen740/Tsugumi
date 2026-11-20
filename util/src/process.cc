#include "tsugumi/util/process.hh"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string_view>

namespace tsugumi::proc {

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
  for (auto [child, parent] : spec.fds) mapped[nmapped++] = {child, parent};
  int err_pipe[2];
  if (::pipe(err_pipe) != 0) throw std::runtime_error("pipe failed");
  ::fcntl(err_pipe[1], F_SETFD, FD_CLOEXEC);
  pid_t pid = ::fork();
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
            ::write(err_pipe[1],
                    reinterpret_cast<const char*>(&ce) + written,
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
#ifdef SYS_close_range
    int from = 3;
    for (int fd = 3; fd <= 1024 + 64; ++fd)
      if (keep(fd)) {
        if (fd > from) ::syscall(SYS_close_range, from, fd - 1, 0);
        from = fd + 1;
      }
    ::syscall(SYS_close_range, from, ~0u, 0);
#else
    long max = ::sysconf(_SC_OPEN_MAX);
    for (int fd = 3; fd < (max > 0 ? max : 65536); ++fd)
      if (!keep(fd)) ::close(fd);
#endif
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
    ssize_t n = ::read(err_pipe[0],
                       reinterpret_cast<char*>(&child_err) + got,
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
           std::chrono::milliseconds grace) {
  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (::pipe(out_pipe) != 0) throw std::runtime_error("pipe failed");
  if (::pipe(err_pipe) != 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    throw std::runtime_error("pipe failed");
  }
  for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]})
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  Spec spec = spec_in;
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
  bool out_open = true;
  bool err_open = true;
  std::optional<std::chrono::steady_clock::time_point> kill_at;
  bool term_sent = false;
  while (out_open || err_open) {
    pollfd fds[2] = {{out_open ? out_pipe[0] : -1, POLLIN, 0},
                     {err_open ? err_pipe[0] : -1, POLLIN, 0}};
    ::poll(fds, 2, 100);
    char buf[8192];
    if (out_open && (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
      ssize_t r = ::read(out_pipe[0], buf, sizeof buf);
      if (r < 0 && errno == EINTR) {
      } else if (r <= 0)
        out_open = false;
      else
        res.out.append(buf, static_cast<size_t>(r));
    }
    if (err_open && (fds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
      ssize_t r = ::read(err_pipe[0], buf, sizeof buf);
      if (r < 0 && errno == EINTR) {
      } else if (r <= 0) {
        err_open = false;
      } else if (on_stderr && !log_error && !truncated) {
        std::string_view remaining(buf, static_cast<size_t>(r));
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
          } else if (pending.empty()) {
            lines.emplace_back(fragment);
          } else {
            pending.append(fragment);
            lines.push_back(std::move(pending));
            pending.clear();
          }
        }
        deliver({lines, truncated});
        lines.clear();
      }
    }
    auto now = std::chrono::steady_clock::now();
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
  if (!pending.empty() && !log_error) deliver({std::span(&pending, 1)});
  ::close(out_pipe[0]);
  ::close(err_pipe[0]);
  res.status = wait(pid);
  // Make sure no orphaned grandchildren survive a cancelled build.
  if (term_sent) kill_group(pid, SIGKILL);
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
