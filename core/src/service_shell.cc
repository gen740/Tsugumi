#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
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

auto Execute(const std::string& command, const std::string& cwd,
             uint32_t timeout, const std::atomic<bool>& cancelled)
    -> ShellOutput {
  Pipe out;
  Pipe err;
  proc::Spec spec;
  spec.argv = {TSUGUMI_BASH, "--noprofile", "--norc", "-c", command};
  spec.cwd = cwd;
  spec.stdout_fd = out.write;
  spec.stderr_fd = err.write;
  // The VM launcher supplies the project's environment. Never forward model
  // credentials or arbitrary host environment variables to a command.
  spec.env = {std::string("PATH=") +
              (std::getenv("PATH") ? std::getenv("PATH") : TSUGUMI_CARD_PATH)};
  for (const char* key :
       {"HOME", "TMPDIR", "USER", "LANG", "NIX_REMOTE", "NIX_SSL_CERT_FILE",
        "SSL_CERT_FILE", "TSUGUMI_PROJECT_ID"})
    if (const char* value = std::getenv(key))
      spec.env.push_back(std::string(key) + "=" + value);
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
        Execute(command, opts_.root.string(), timeout, cancelled);
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
