#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef __APPLE__
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <stdexcept>

#include "service.hh"
#include "util/process.hh"

namespace tsugumi {
namespace {

namespace R = Tsugumi::RPC;
using FBB = flatbuffers::FlatBufferBuilder;
using Clock = std::chrono::steady_clock;

struct Pipe {
  int read = -1;
  int write = -1;
  Pipe() {
    int fds[2];
    if (::pipe(fds) != 0)
      throw std::runtime_error("cannot create shell output pipe");
    read = fds[0];
    write = fds[1];
    ::fcntl(read, F_SETFD, FD_CLOEXEC);
    ::fcntl(write, F_SETFD, FD_CLOEXEC);
    ::fcntl(read, F_SETFL, ::fcntl(read, F_GETFL) | O_NONBLOCK);
  }
  Pipe(const Pipe&) = delete;
  auto operator=(const Pipe&) -> Pipe& = delete;
  ~Pipe() {
    if (read >= 0) ::close(read);
    if (write >= 0) ::close(write);
  }
  auto CloseWriter() -> void {
    ::close(write);
    write = -1;
  }
};

struct ShellOutput {
  std::array<std::string, 2> streams;
  std::array<bool, 2> truncated{};
  int status = 0;
  bool cancelled = false;
  bool timed_out = false;
};

// Diagnostic scratch belongs to one command, never to the shared host /tmp.
struct ShellTemporary {
  std::filesystem::path path;
  ~ShellTemporary() {
    if (!path.empty()) {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  }
};

auto Execute(const std::string& command, const std::string& cwd,
             uint32_t timeout, const std::atomic<bool>& cancelled, bool agent,
             const std::filesystem::path& control)
    -> ShellOutput {
  Pipe out;
  Pipe err;
  ShellTemporary temporary;
#ifndef __APPLE__
  std::optional<Pipe> socket_filter;
#endif
  proc::Spec spec;
  spec.argv = {TSUGUMI_BASH, "--noprofile", "--norc", "-c", command};
  if (agent) {
    // A read-only filesystem alone still exposes authenticated RPC and host
    // credentials. Hide those too, and prevent Unix socket connections even
    // when a caller knows an endpoint outside the usual control directory.
    const auto root = std::filesystem::canonical(cwd).string();
    std::vector<std::string> hidden;
    for (const auto& path : {control, std::filesystem::path("/control"),
                            std::filesystem::path("/run/tsugumi")})
      if (!path.empty() && std::filesystem::exists(path))
        hidden.push_back(std::filesystem::canonical(path).string());
    const char* home = std::getenv("HOME");
    if (home && *home && std::filesystem::exists(home))
      hidden.push_back(std::filesystem::canonical(home).string());
#ifdef __APPLE__
    if (::access("/usr/bin/sandbox-exec", X_OK) != 0)
      throw Failed(FailureKind::permission_denied,
                   "Agent shell reads require Seatbelt");
    auto quote = [](const std::string& path) {
      std::string out = "\"";
      for (const char c : path) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
      }
      return out + "\"";
    };
    const auto shared_tmp = std::filesystem::canonical(
        std::filesystem::temp_directory_path()).string();
    auto pattern = shared_tmp + "/tsugumi-shell-XXXXXX";
    if (!::mkdtemp(pattern.data()))
      throw std::runtime_error("cannot create Agent shell scratch");
    temporary.path = pattern;
    std::string profile = "(version 1)(allow default)(deny file-write*)";
    for (const auto& path : hidden)
      profile += "(deny file-read* (subpath " + quote(path) + "))";
    profile += "(deny file-read* (subpath \"/private/tmp\")"
               " (subpath \"/private/var/tmp\") (subpath " + quote(shared_tmp) + "))";
    profile += "(allow file-read* (subpath " + quote(root) + ") (subpath " + quote(pattern) + "))"
               "(allow file-write* (subpath " + quote(pattern) + ") (literal \"/dev/null\"))";
    // The control directory remains hidden even if it is under the project.
    if (!control.empty())
      profile += "(deny file-read* (subpath " +
                 quote(std::filesystem::weakly_canonical(control).string()) + "))";
    profile += "(deny network-outbound (remote unix-socket))"
               "(deny network-outbound (remote ip \"localhost:*\"))"
               "(allow network-outbound (remote unix-socket (path-literal \"/private/var/run/mDNSResponder\")))"
               "(deny mach-lookup)"
               "(allow mach-lookup (global-name \"com.apple.trustd.agent\"))"
               "(deny signal)(allow signal (target same-sandbox))";
    spec.argv.insert(spec.argv.begin(), {"/usr/bin/sandbox-exec", "-p", profile});
#else
    const std::string bwrap = TSUGUMI_BWRAP;
    if (bwrap.empty() || ::access(bwrap.c_str(), X_OK) != 0)
      throw Failed(FailureKind::permission_denied,
                   "Agent shell reads require bubblewrap");
    // Keep external IP networking, but reject all Unix socket creation,
    // including abstract sockets and endpoints at arbitrary host paths.
#if defined(__aarch64__)
    constexpr auto kArchitecture = AUDIT_ARCH_AARCH64;
#elif defined(__x86_64__)
    constexpr auto kArchitecture = AUDIT_ARCH_X86_64;
#else
#error Unsupported Agent shell seccomp architecture
#endif
    const sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kArchitecture, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        // Do not allow x32 syscall numbers to bypass the native syscall check.
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#ifdef __NR_io_uring_setup
        // io_uring can create/connect sockets without the socket syscall.
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_io_uring_setup, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socketpair, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
    socket_filter.emplace();
    if (::write(socket_filter->write, filter, sizeof filter) !=
        static_cast<ssize_t>(sizeof filter))
      throw std::runtime_error("cannot prepare Agent shell socket filter");
    socket_filter->CloseWriter();
    spec.fds = {{3, socket_filter->read}};
    std::vector<std::string> isolation{
        bwrap, "--unshare-user", "--unshare-pid", "--unshare-ipc",
        "--die-with-parent", "--new-session", "--cap-drop", "ALL",
        "--ro-bind", "/", "/", "--tmpfs", "/tmp", "--tmpfs", "/var/tmp"};
    for (const auto& path : hidden)
      isolation.insert(isolation.end(), {"--tmpfs", path});
    isolation.insert(isolation.end(), {"--ro-bind", root, root});
    // A control directory inside the project must be covered after root.
    if (!control.empty())
      isolation.insert(isolation.end(),
                       {"--tmpfs", std::filesystem::weakly_canonical(control).string()});
    isolation.insert(isolation.end(), {"--proc", "/proc", "--dev", "/dev",
                                       "--seccomp", "3", "--"});
    spec.argv.insert(spec.argv.begin(), isolation.begin(), isolation.end());
#endif
  }
  spec.cwd = cwd;
  spec.stdout_fd = out.write;
  spec.stderr_fd = err.write;
  // The VM launcher supplies the project's environment. Never forward model
  // credentials or arbitrary host environment variables to a command.
  spec.env = {std::string("PATH=") +
              (std::getenv("PATH") ? std::getenv("PATH") : TSUGUMI_CARD_PATH)};
  for (const char* key :
       {"USER", "LANG", "NIX_SSL_CERT_FILE",
        "SSL_CERT_FILE", "TSUGUMI_PROJECT_ID"})
    if (const char* value = std::getenv(key))
      spec.env.push_back(std::string(key) + "=" + value);
  if (agent) {
    const auto scratch = temporary.path.empty() ? "/tmp" : temporary.path.string();
    spec.env.push_back("HOME=" + scratch);
    spec.env.push_back("TMPDIR=" + scratch);
  } else {
    for (const char* key : {"HOME", "TMPDIR", "NIX_REMOTE"})
      if (const char* value = std::getenv(key))
        spec.env.push_back(std::string(key) + "=" + value);
  }
  const auto pid = proc::spawn(spec);
  struct Child {
    pid_t pid;
    bool reaped = false;
    ~Child() {
      if (!reaped) {
        proc::kill_group(pid, SIGKILL);
        proc::wait(pid);
      }
    }
  } child{pid};
  out.CloseWriter();
  err.CloseWriter();
  ShellOutput result;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
  std::optional<Clock::time_point> kill_at;
  std::optional<Clock::time_point> drain_until;
  std::array<bool, 2> open{true, true};
  constexpr size_t kOutputLimit = 262144;
  while (true) {
    const auto now = Clock::now();
    if (!kill_at && !child.reaped && (cancelled.load() || now >= deadline)) {
      result.cancelled = cancelled.load();
      result.timed_out = now >= deadline;
      proc::kill_group(pid, SIGTERM);
      kill_at = now + std::chrono::milliseconds(250);
    }
    if (!child.reaped && kill_at && now >= *kill_at)
      proc::kill_group(pid, SIGKILL);
    if (!child.reaped) {
      if (auto status = proc::try_wait(pid)) {
        result.status = *status;
        child.reaped = true;
        // Background children do not outlive a command. A pipe inherited by
        // another process cannot keep the RPC waiting indefinitely either.
        proc::kill_group(pid, SIGKILL);
        drain_until = now + std::chrono::milliseconds(100);
      }
    }
    if (child.reaped && !open[0] && !open[1]) break;
    if (child.reaped && now >= *drain_until) {
      for (size_t index = 0; index < 2; ++index)
        result.truncated[index] = result.truncated[index] || open[index];
      break;
    }
    pollfd fds[2] = {{open[0] ? out.read : -1, POLLIN, 0},
                     {open[1] ? err.read : -1, POLLIN, 0}};
    if (::poll(fds, 2, 20) < 0 && errno != EINTR)
      throw std::runtime_error("shell output poll failed");
    for (size_t index = 0; index < 2; ++index) {
      if (!open[index] || !(fds[index].revents & (POLLIN | POLLHUP | POLLERR)))
        continue;
      // Read at most one chunk per stream per iteration: a writer flooding a
      // pipe must not starve deadline or cancellation checks.
      char buffer[8192];
      const auto count = ::read(fds[index].fd, buffer, sizeof buffer);
      if (count == 0)
        open[index] = false;
      else if (count > 0) {
        auto& output = result.streams[index];
        const auto keep =
            std::min(static_cast<size_t>(count), kOutputLimit - output.size());
        output.append(buffer, keep);
        result.truncated[index] =
            result.truncated[index] || keep < static_cast<size_t>(count);
      } else if (errno != EAGAIN && errno != EINTR) {
        throw std::runtime_error("cannot read shell output");
      }
    }
  }
  return result;
}

auto ShellReceipt(FBB& b, const std::string& operation, R::ReceiptValue type,
                  flatbuffers::Offset<void> value) -> std::string {
  const auto receipt =
      R::CreateOperationReceipt(b, b.CreateString(operation), type, value);
  b.Finish(receipt);
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}

}  // namespace

auto Service::dispatch_shell(RequestIn req) -> void {
  const auto key = std::make_pair(req.conn, req.request_id);
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  {
    std::lock_guard lock(shell_mu_);
    if (shell_stopping_) {
      fail(req, FailureData::make(FailureKind::cancelled,
                                  "the project is stopping"));
      return;
    }
    shell_cancellations_[key] = cancelled;
  }
  const bool accepted = shell_lane_->submit([this, req, cancelled,
                                             key](bool expired) mutable {
    try {
      if (expired)
        throw Failed(FailureKind::timeout,
                     "the shell request expired before starting");
      if (cancelled->load())
        throw Failed(FailureKind::cancelled,
                     "the shell request was cancelled before starting");
      project_shell(req, *cancelled);
    } catch (const Failed& error) {
      fail(req, error.failure);
    } catch (const std::exception& error) {
      fail(req, FailureData::make(FailureKind::internal_error, error.what()));
    }
    std::lock_guard lock(shell_mu_);
    shell_cancellations_.erase(key);
  });
  if (!accepted) {
    std::lock_guard lock(shell_mu_);
    shell_cancellations_.erase(key);
    fail(req, FailureData::make(FailureKind::busy,
                                "the shell request queue is full"));
  }
}

auto Service::project_shell(RequestIn& req, const std::atomic<bool>& cancelled)
    -> void {
  if (!opts_.project_shell)
    throw Failed(FailureKind::unsupported,
                 "shell commands are available only in a launcher-enabled "
                 "isolated project environment");
  if (::geteuid() == 0)
    throw Failed(FailureKind::permission_denied,
                 "shell commands must run as an unprivileged project user");
  const auto* request = req.request()->value_as_ProjectShellRequest();
  const auto command = request->command()->str();
  const auto timeout = request->timeout_ms().value_or(10000);
  if (timeout == 0 || timeout > 30000)
    throw Failed(FailureKind::invalid_request,
                 "timeout_ms must be between 1 and 30000");
  if (command.empty() || command.size() > 65536 ||
      command.find('\0') != std::string::npos)
    throw Failed(FailureKind::invalid_request,
                 "command must contain 1 to 65536 bytes without NUL");
  const auto operation = request->op_id()->str();
  const auto digest = request_digest(req.request());
  if (replay_or_join(req, operation, "shell", digest)) return;
  // Save an uncertainty receipt BEFORE spawning. If this daemon crashes, a
  // retry can report uncertainty but can never execute the command twice.
  FBB marker;
  const auto failure = wire::failure(
      marker, FailureData::make(
                  FailureKind::interrupted,
                  "shell command started but no final result was recorded; "
                  "inspect its effects before starting a new operation"));
  const auto started = ShellReceipt(marker, operation, R::ReceiptValue::Failure,
                                    failure.Union());
  bool recorded = false;
  try {
    history_->write_sync([&](sql::Db& db) {
      History::put_receipt(db, operation, "shell", digest, started);
    });
    recorded = true;
    const auto output =
        Execute(command, opts_.root.string(), timeout, cancelled,
                request->origin()->str() == "agent", opts_.control_dir);
    FBB builder;
    const auto out = builder.CreateVector(
        reinterpret_cast<const uint8_t*>(output.streams[0].data()),
        output.streams[0].size());
    const auto err = builder.CreateVector(
        reinterpret_cast<const uint8_t*>(output.streams[1].data()),
        output.streams[1].size());
    const auto result = R::CreateProjectShellResult(
        builder, out, err,
        WIFEXITED(output.status) ? WEXITSTATUS(output.status) : -1,
        WIFSIGNALED(output.status) ? WTERMSIG(output.status) : 0,
        output.cancelled, output.timed_out, output.truncated[0],
        output.truncated[1]);
    const auto bytes =
        ShellReceipt(builder, operation, R::ReceiptValue::ProjectShellResult,
                     result.Union());
    history_->write_sync([&](sql::Db& db) {
      History::put_receipt(db, operation, "shell", digest, bytes);
    });
    finish_op(operation, bytes);
    reply(req, receipt_reply(req.request_id, bytes, false));
  } catch (...) {
    // Once spawning was possible, keep the durable uncertainty marker even
    // if saving the final result failed. Do not make this operation retryable.
    if (recorded)
      finish_op(operation, started);
    else
      store_failure_receipt(
          operation, "shell", digest,
          FailureData::make(FailureKind::persistence_failed,
                            "cannot record shell operation before execution"));
    throw;
  }
}

}  // namespace tsugumi
