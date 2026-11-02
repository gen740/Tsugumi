#include "util/process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace tsugumi::proc {

pid_t spawn(const Spec& spec) {
  if (spec.argv.empty()) throw std::invalid_argument("empty argv");
  std::vector<char*> argv, envp;
  for (const auto& a : spec.argv) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  for (const auto& e : spec.env) envp.push_back(const_cast<char*>(e.c_str()));
  envp.push_back(nullptr);
  int err_pipe[2];
  if (::pipe(err_pipe) != 0) throw std::runtime_error("pipe failed");
  ::fcntl(err_pipe[1], F_SETFD, FD_CLOEXEC);
  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    throw std::runtime_error("fork failed");
  }
  if (pid == 0) {
    ::close(err_pipe[0]);
    ::setpgid(0, 0);
    // Reset signal dispositions the daemon changed.
    ::signal(SIGPIPE, SIG_DFL);
    ::signal(SIGCHLD, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
    int devnull = ::open("/dev/null", O_RDWR);
    ::dup2(devnull, 0);
    ::dup2(spec.stdout_fd >= 0 ? spec.stdout_fd : devnull, 1);
    ::dup2(spec.stderr_fd >= 0 ? spec.stderr_fd : devnull, 2);
    // Move mapped fds to high numbers first to avoid clobbering.
    std::vector<std::pair<int, int>> moved;
    for (auto [child, parent] : spec.fds) {
      int tmp = ::fcntl(parent, F_DUPFD, 64);
      moved.emplace_back(child, tmp);
    }
    for (auto [child, tmp] : moved) {
      ::dup2(tmp, child);
      ::close(tmp);
    }
    // Close everything else.
    for (int fd = 3; fd < 1024; ++fd) {
      bool keep = fd == err_pipe[1];
      for (auto [child, _] : spec.fds) keep |= fd == child;
      if (!keep) ::close(fd);
    }
    if (!spec.cwd.empty() && ::chdir(spec.cwd.c_str()) != 0) {
      int e = errno;
      (void)::write(err_pipe[1], &e, sizeof e);
      ::_exit(127);
    }
    if (spec.gid && ::setgid(*spec.gid) != 0) {
      int e = errno;
      (void)::write(err_pipe[1], &e, sizeof e);
      ::_exit(127);
    }
    if (spec.uid && ::setuid(*spec.uid) != 0) {
      int e = errno;
      (void)::write(err_pipe[1], &e, sizeof e);
      ::_exit(127);
    }
    ::execve(argv[0], argv.data(), envp.data());
    int e = errno;
    (void)::write(err_pipe[1], &e, sizeof e);
    ::_exit(127);
  }
  ::close(err_pipe[1]);
  ::setpgid(pid, pid);
  int child_errno = 0;
  ssize_t n = ::read(err_pipe[0], &child_errno, sizeof child_errno);
  ::close(err_pipe[0]);
  if (n == sizeof child_errno) {
    ::waitpid(pid, nullptr, 0);
    throw std::runtime_error("cannot start " + spec.argv[0] + ": " + std::strerror(child_errno));
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
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  return status;
}

Result run(const Spec& spec_in, const std::atomic<bool>* cancel,
           std::optional<std::chrono::steady_clock::time_point> deadline,
           const std::function<void(const std::string&)>& on_stderr, std::chrono::milliseconds grace) {
  int out_pipe[2], err_pipe[2];
  if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) throw std::runtime_error("pipe failed");
  Spec spec = spec_in;
  spec.stdout_fd = out_pipe[1];
  spec.stderr_fd = err_pipe[1];
  pid_t pid;
  try {
    pid = spawn(spec);
  } catch (...) {
    for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) ::close(fd);
    throw;
  }
  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  Result res;
  std::string err_line;
  bool out_open = true, err_open = true;
  std::optional<std::chrono::steady_clock::time_point> kill_at;
  bool term_sent = false;
  while (out_open || err_open) {
    pollfd fds[2] = {{out_open ? out_pipe[0] : -1, POLLIN, 0}, {err_open ? err_pipe[0] : -1, POLLIN, 0}};
    ::poll(fds, 2, 100);
    char buf[8192];
    if (out_open && (fds[0].revents & (POLLIN | POLLHUP))) {
      ssize_t r = ::read(out_pipe[0], buf, sizeof buf);
      if (r <= 0) out_open = false;
      else res.out.append(buf, static_cast<size_t>(r));
    }
    if (err_open && (fds[1].revents & (POLLIN | POLLHUP))) {
      ssize_t r = ::read(err_pipe[0], buf, sizeof buf);
      if (r <= 0) {
        err_open = false;
      } else {
        err_line.append(buf, static_cast<size_t>(r));
        size_t nl;
        while ((nl = err_line.find('\n')) != std::string::npos) {
          if (on_stderr) on_stderr(err_line.substr(0, nl + 1));
          err_line.erase(0, nl + 1);
        }
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
  if (!err_line.empty() && on_stderr) on_stderr(err_line);
  ::close(out_pipe[0]);
  ::close(err_pipe[0]);
  res.status = wait(pid);
  // Make sure no orphaned grandchildren survive a cancelled build.
  if (term_sent) kill_group(pid, SIGKILL);
  return res;
}

bool exited_ok(int status) { return WIFEXITED(status) && WEXITSTATUS(status) == 0; }

std::string describe(int status) {
  if (WIFEXITED(status)) return "exit status " + std::to_string(WEXITSTATUS(status));
  if (WIFSIGNALED(status)) return std::string("signal ") + std::to_string(WTERMSIG(status));
  return "unknown status";
}

}  // namespace tsugumi::proc
