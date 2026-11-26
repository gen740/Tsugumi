#include "host/native_terminal.hh"

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#include <sys/syscall.h>
#endif

#include <array>
#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "host/descriptor.hh"

namespace tsugumi::host {
namespace H = Tsugumi::Host;
namespace {
auto TerminalAccount() -> NativeTerminals::Account {
  passwd user{};
  passwd* found = nullptr;
  std::vector<char> buffer(16384);
  if (::getpwuid_r(::getuid(), &user, buffer.data(), buffer.size(), &found) ||
      !found)
    throw std::runtime_error("Cannot determine the terminal account");
  return {user.pw_dir, user.pw_name,
          user.pw_shell[0] ? user.pw_shell : "/bin/bash"};
}
}  // namespace
struct NativeTerminals::Impl {
  struct Session {
    int fd = -1;
    pid_t pid = -1;
    std::mutex io;
    std::atomic<bool> finished = false;
    int wake[2] = {-1, -1};
    std::atomic<bool> closing = false;
    std::jthread reader;
    auto Close() -> void {
      if (closing.exchange(true)) return;
      if (wake[1] >= 0) (void)::write(wake[1], "x", 1);
      if (pid > 0) ::kill(-pid, SIGHUP);
    }
    ~Session() {
      Close();
      if (reader.joinable()) reader.join();
      if (fd >= 0) ::close(fd);
      for (auto pipe : wake)
        if (pipe >= 0) ::close(pipe);
      if (pid > 0) {
        ::kill(-pid, SIGKILL);
        ::kill(pid, SIGKILL);
        while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
        }
      }
    }
  };
  Output output;
  AccountLookup account;
  std::mutex mutex;
  std::map<uint64_t, std::shared_ptr<Session>> sessions;
  uint64_t next = 0;
  bool stopping = false;
  Impl(Output value, AccountLookup lookup)
      : output(std::move(value)),
        account(lookup ? std::move(lookup) : TerminalAccount) {}
};
NativeTerminals::NativeTerminals(Output output, AccountLookup account)
    : impl_(std::make_unique<Impl>(std::move(output), std::move(account))) {}
NativeTerminals::~NativeTerminals() { Shutdown(); }
auto NativeTerminals::Shutdown() -> void {
  std::map<uint64_t, std::shared_ptr<Impl::Session>> sessions;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->stopping = true;
    sessions.swap(impl_->sessions);
  }
  for (const auto& [_, session] : sessions) session->Close();
  sessions.clear();
}
auto NativeTerminals::Handle(const H::HostTerminalRequestT& request)
    -> H::HostTerminalT {
  auto& s = *impl_;
  std::unique_lock lock(s.mutex);
  if (s.stopping) throw std::runtime_error("Host terminals are stopping");
  H::HostTerminalT result;
  result.session = request.session;
  if (request.action == H::TerminalAction::Open) {
    // Reap completed sessions without holding a lock needed by their readers.
    std::vector<std::shared_ptr<Impl::Session>> completed;
    for (auto i = s.sessions.begin(); i != s.sessions.end();) {
      if (i->second->finished) {
        completed.push_back(i->second);
        i = s.sessions.erase(i);
      } else
        ++i;
    }
    if (s.sessions.size() >= 8)
      throw std::runtime_error("At most 8 host terminals may be open");
    if (!request.rows || !request.cols || request.rows >= 1000 ||
        request.cols >= 1000)
      throw std::runtime_error("Invalid terminal size");
    auto session = std::make_shared<Impl::Session>();
    if (::pipe(session->wake) != 0)
      throw std::runtime_error("Cannot create terminal cancellation pipe");
    for (auto fd : session->wake) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    int slave = -1;
    winsize size{};
    size.ws_row = static_cast<unsigned short>(request.rows);
    size.ws_col = static_cast<unsigned short>(request.cols);
    if (::openpty(&session->fd, &slave, nullptr, nullptr, &size))
      throw std::runtime_error("Cannot create host terminal");
    Descriptor slave_owner(slave);
    ::fcntl(session->fd, F_SETFD, FD_CLOEXEC);
    ::fcntl(session->fd, F_SETFL, O_NONBLOCK);
    ::fcntl(slave, F_SETFD, FD_CLOEXEC);
    // Only terminal identity crosses into the shell; no service credentials,
    // provider keys, execution capabilities or arbitrary host environment.
    const auto user = s.account();
    const auto& home = user.home;
    std::vector<std::string> values = {
        "HOME=" + home,
        "USER=" + user.name,
        "LOGNAME=" + user.name,
        "SHELL=" + user.shell,
        "TMPDIR=/tmp",
        "LANG=C.UTF-8",
        "PATH=" + home +
            "/.nix-profile/bin:/nix/var/nix/profiles/default/bin:"
            "/run/current-system/sw/bin:/usr/bin:/bin:/usr/sbin:/sbin",
        "TERM=xterm-256color",
        "COLORTERM=truecolor",
        "TSUGUMI_PROJECT=" + request.project};
    std::vector<char*> environment;
    for (auto& value : values) environment.push_back(value.data());
    environment.push_back(nullptr);
    const char* shell = user.shell.c_str();
    char* const args[] = {const_cast<char*>(shell), const_cast<char*>("-l"),
                          nullptr};
    const int descriptor_limit = ::getdtablesize();
    const auto pid = ::fork();
    if (pid == 0) {
      ::setsid();
      ::ioctl(slave, TIOCSCTTY, 0);
      ::dup2(slave, 0);
      ::dup2(slave, 1);
      ::dup2(slave, 2);
#ifdef SYS_close_range
      if (::syscall(SYS_close_range, 3u, ~0u, 0) != 0)
#endif
        for (int fd = 3; fd < descriptor_limit; ++fd) ::close(fd);
      sigset_t empty;
      sigemptyset(&empty);
      ::sigprocmask(SIG_SETMASK, &empty, nullptr);
      ::signal(SIGPIPE, SIG_DFL);
      ::signal(SIGCHLD, SIG_DFL);
      if (::chdir(request.project.c_str())) ::_exit(126);
      ::execve(shell, args, environment.data());
      ::_exit(127);
    }
    slave_owner.reset();
    if (pid < 0) throw std::runtime_error("Cannot start host terminal shell");
    session->pid = pid;
    result.session = ++s.next;
    s.sessions[result.session] = session;
    auto* raw = session.get();
    session->reader =
        std::jthread([raw, id = result.session, output = s.output]() {
          std::array<uint8_t, 8192> bytes{};
          while (!raw->closing) {
            pollfd fds[] = {{raw->fd, POLLIN, 0}, {raw->wake[0], POLLIN, 0}};
            const auto ready = ::poll(fds, 2, -1);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || fds[1].revents) break;
            const auto count = ::read(raw->fd, bytes.data(), bytes.size());
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) break;
            H::HostTerminalDataT event;
            event.session = id;
            event.data.assign(bytes.begin(), bytes.begin() + count);
            if (output) output(event);
          }
          H::HostTerminalDataT event;
          event.session = id;
          event.closed = true;
          raw->Close();
          if (output) output(event);
          raw->finished = true;
        });
    lock.unlock();
    return result;
  }
  const auto found = s.sessions.find(request.session);
  if (found == s.sessions.end()) {
    if (request.action == H::TerminalAction::Close) return result;
    throw std::runtime_error("Unknown host terminal session");
  }
  auto session = found->second;
  if (request.action == H::TerminalAction::Close) {
    s.sessions.erase(found);
    lock.unlock();
    session->Close();
    session.reset();
    return result;
  }
  lock.unlock();
  std::lock_guard io(session->io);
  if (session->finished) throw std::runtime_error("Host terminal has exited");
  if (request.action == H::TerminalAction::Resize) {
    if (!request.rows || !request.cols || request.rows >= 1000 ||
        request.cols >= 1000)
      throw std::runtime_error("Invalid terminal size");
    winsize size{};
    size.ws_row = static_cast<unsigned short>(request.rows);
    size.ws_col = static_cast<unsigned short>(request.cols);
    if (::ioctl(session->fd, TIOCSWINSZ, &size))
      throw std::runtime_error("Cannot resize host terminal");
  } else if (request.action == H::TerminalAction::Input) {
    size_t offset = 0;
    while (offset < request.data.size()) {
      pollfd fds[] = {{session->fd, POLLOUT, 0}, {session->wake[0], POLLIN, 0}};
      const auto ready = ::poll(fds, 2, -1);
      if (ready < 0 && errno == EINTR) continue;
      if (ready < 0 || fds[1].revents ||
          (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)))
        throw std::runtime_error("Host terminal closed during input");
      const auto written = ::write(session->fd, request.data.data() + offset,
                                   request.data.size() - offset);
      if (written < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      if (written <= 0)
        throw std::runtime_error("Cannot write host terminal input");
      offset += static_cast<size_t>(written);
    }
  } else
    throw std::runtime_error("Unknown terminal action");
  return result;
}
}  // namespace tsugumi::host
