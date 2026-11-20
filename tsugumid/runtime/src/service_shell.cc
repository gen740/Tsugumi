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
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#include "service.hh"
#include "shell.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/process.hh"

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

}  // namespace

auto shell::Execute(const std::string& command, const std::string& cwd,
                    uint32_t timeout, const std::atomic<bool>& cancelled,
                    bool read_only, bool hide_credentials,
                    bool no_project_socket,
                    const std::filesystem::path& control, const View& view)
    -> Output {
  Pipe out;
  Pipe err;
  ShellTemporary temporary;
#ifndef __APPLE__
  std::optional<Pipe> socket_filter;
#endif
  proc::Spec spec;
  spec.argv = {TSUGUMI_BASH, "--noprofile", "--norc", "-c", command};
  if (read_only || hide_credentials || no_project_socket) {
    // Each requested isolation mechanism is independent of caller identity.
    const auto root = std::filesystem::canonical(cwd).string();
    std::vector<std::string> hidden;
    if (hide_credentials)
      for (const auto& path : {control, std::filesystem::path("/control"),
                               std::filesystem::path("/run/tsugumi")})
        if (!path.empty() && std::filesystem::exists(path))
          hidden.push_back(std::filesystem::canonical(path).string());
    const char* home = std::getenv("HOME");
    if (hide_credentials && home && *home && std::filesystem::exists(home))
      hidden.push_back(std::filesystem::canonical(home).string());
    const auto shared_tmp =
        std::filesystem::canonical(std::filesystem::temp_directory_path())
            .string();
    auto pattern = shared_tmp + "/tsugumi-shell-XXXXXX";
    if (!::mkdtemp(pattern.data()))
      throw std::runtime_error("cannot create Isolated shell scratch");
    temporary.path = pattern;
    // A store also contains cached Card definitions. Expose only the closure
    // of the launcher's tools, never every previously built source directory.
    std::set<std::string> runtime_store;
    std::vector<std::pair<std::string, std::string>> runtime_aliases;
    if (!view.hidden_root.empty()) {
      auto add_runtime = [&](const std::filesystem::path& path, bool alias) {
        std::error_code error;
        const auto resolved = std::filesystem::canonical(path, error).string();
        if (error || !resolved.starts_with("/nix/store/")) return;
        runtime_store.insert(resolved.substr(0, resolved.find('/', 11)));
        if (alias) runtime_aliases.emplace_back(resolved, path.string());
      };
      add_runtime(TSUGUMI_BASH, false);
      for (const auto* name : {"SSL_CERT_FILE", "NIX_SSL_CERT_FILE"})
        if (const auto* certificate = std::getenv(name))
          add_runtime(certificate, false);
      std::vector<std::string> search;
      std::istringstream paths(std::getenv("PATH") ? std::getenv("PATH") : "");
      std::string path;
      while (std::getline(paths, path, ':'))
        if (!path.empty()) {
          search.push_back(path);
          add_runtime(path, true);
        }
      if (!runtime_store.empty()) {
        search.push_back("/nix/var/nix/profiles/default/bin");
        std::string query;
        for (const auto& directory : search)
          if (::access((directory + "/nix-store").c_str(), X_OK) == 0) {
            query = directory + "/nix-store";
            break;
          }
        if (query.empty())
          throw Failed(FailureKind::permission_denied,
                       "cannot resolve the shell tool runtime closure");
        proc::Spec closure;
        closure.argv = {query, "--query", "--requisites"};
        closure.argv.insert(closure.argv.end(), runtime_store.begin(),
                            runtime_store.end());
        closure.env = {"HOME=" + temporary.path.string()};
        const auto result =
            proc::run(closure, &cancelled,
                      Clock::now() + std::chrono::milliseconds(timeout), {});
        if (!proc::exited_ok(result.status))
          throw Failed(FailureKind::permission_denied,
                       "cannot resolve the shell tool runtime closure");
        runtime_store.clear();
        std::istringstream output(result.out);
        while (std::getline(output, path)) {
          if (!path.starts_with("/nix/store/") ||
              path.find('/', 11) != std::string::npos)
            throw Failed(FailureKind::invalid_path,
                         "invalid runtime closure path");
          runtime_store.insert(path);
        }
      }
    }
#ifdef __APPLE__
    if (::access("/usr/bin/sandbox-exec", X_OK) != 0)
      throw Failed(FailureKind::permission_denied,
                   "Isolated shell reads require Seatbelt");
    auto quote = [](const std::string& path) {
      std::string out = "\"";
      for (const char c : path) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
      }
      return out + "\"";
    };
    std::string profile = "(version 1)(allow default)";
    if (read_only) profile += "(deny file-write*)";
    for (const auto& path : hidden)
      profile += "(deny file-read* (subpath " + quote(path) + "))";
    if (hide_credentials)
      profile +=
          "(deny file-read* (subpath \"/private/tmp\")"
          " (subpath \"/private/var/tmp\") (subpath " +
          quote(shared_tmp) + "))";
    if (!view.hidden_root.empty()) {
      // Every process reads the root directory itself while it starts, and
      // aborts without it. Its data alone does not let a command list it.
      profile += "(deny file-read*)(allow file-read-data (literal \"/\"))";
      for (const auto* runtime :
           {"/usr/bin", "/usr/sbin", "/usr/lib", "/usr/share", "/bin", "/sbin",
            "/Library/Apple/System", "/System", "/dev", "/private/etc/ssl",
            "/private/etc/resolv.conf", "/private/etc/hosts"})
        profile += "(allow file-read* (subpath " + quote(runtime) + "))";
      for (const auto& runtime : runtime_store)
        profile += "(allow file-read* (subpath " + quote(runtime) + "))";
      for (const auto& [source, target] : runtime_aliases)
        profile += "(allow file-read* (subpath " + quote(target) + "))";
    }
    profile += "(allow file-read* (subpath " + quote(root) + ") (subpath " +
               quote(pattern) +
               "))"
               "(allow file-write* (subpath " +
               quote(pattern) + ") (literal \"/dev/null\"))";
    for (const auto& path : view.writable)
      profile += "(allow file-write* (subpath " + quote(path.string()) + "))";
    for (const auto& mount : view.mounts) {
      profile +=
          "(allow file-read* (subpath " + quote(mount.source.string()) + "))";
      if (mount.writable)
        profile += "(allow file-write* (subpath " +
                   quote(mount.source.string()) + "))";
    }
    for (const auto& path : view.protected_files)
      profile += "(deny file-write* (literal " + quote(path.string()) + "))";
    for (const auto& path : view.excluded) {
      profile += "(deny file-read* file-write* (subpath " +
                 quote(path.string()) + "))";
    }
    // The control directory remains hidden even if it is under the project.
    if (hide_credentials && !control.empty())
      profile += "(deny file-read* (subpath " +
                 quote(std::filesystem::weakly_canonical(control).string()) +
                 "))";
    if (no_project_socket)
      profile +=
          "(deny network-outbound (remote unix-socket))"
          "(deny network-outbound (remote ip \"localhost:*\"))"
          "(allow network-outbound (remote unix-socket (path-literal "
          "\"/private/var/run/mDNSResponder\")))"
          "(deny mach-lookup)"
          "(allow mach-lookup (global-name \"com.apple.trustd.agent\"))"
          "(deny signal)(allow signal (target same-sandbox))";
    spec.argv.insert(spec.argv.begin(),
                     {"/usr/bin/sandbox-exec", "-p", profile});
#else
    const std::string bwrap = TSUGUMI_BWRAP;
    if (bwrap.empty() || ::access(bwrap.c_str(), X_OK) != 0)
      throw Failed(FailureKind::permission_denied,
                   "Isolated shell reads require bubblewrap");
    // Keep external IP networking and private stream pairs, but reject Unix
    // socket creation for abstract and filesystem endpoints on the host.
#if defined(__aarch64__)
    constexpr auto kArchitecture = AUDIT_ARCH_AARCH64;
#elif defined(__x86_64__)
    constexpr auto kArchitecture = AUDIT_ARCH_X86_64;
#else
#error Unsupported Isolated shell seccomp architecture
#endif
    if (no_project_socket) {
      const sock_filter filter[] = {
          BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
          BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kArchitecture, 1, 0),
          BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
          BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
          // Do not allow x32 syscall numbers to bypass the native syscall
          // check.
          BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1),
          BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#ifdef __NR_io_uring_setup
          // io_uring can create/connect sockets without the socket syscall.
          BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_io_uring_setup, 0, 1),
          BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
          // Stream pairs support local event loops without an endpoint to dial.
          // Datagram pairs can reconnect/sendto arbitrary host Unix addresses.
          BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socketpair, 0, 5),
          BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[1])),
          BPF_STMT(BPF_ALU | BPF_AND | BPF_K,
                   ~static_cast<unsigned>(SOCK_CLOEXEC | SOCK_NONBLOCK)),
          BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SOCK_STREAM, 1, 0),
          BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
          BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
          BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 3),
          BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])),
          BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 0, 1),
          BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
          BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
      socket_filter.emplace();
      if (::write(socket_filter->write, filter, sizeof filter) !=
          static_cast<ssize_t>(sizeof filter))
        throw std::runtime_error("cannot prepare Isolated shell socket filter");
      socket_filter->CloseWriter();
      spec.fds = {{3, socket_filter->read}};
    }
    std::vector<std::string> isolation{bwrap,
                                       "--unshare-user",
                                       "--unshare-pid",
                                       "--unshare-ipc",
                                       "--die-with-parent",
                                       "--new-session",
                                       "--cap-drop",
                                       "ALL",
                                       read_only ? "--ro-bind" : "--bind",
                                       "/",
                                       "/"};
    if (!view.hidden_root.empty()) {
      isolation.resize(isolation.size() - 3);
      isolation.insert(isolation.end(), {"--tmpfs", "/"});
      for (const auto* runtime :
           {"/usr/bin", "/usr/sbin", "/usr/lib", "/usr/share", "/bin", "/sbin",
            "/lib", "/lib64", "/etc/ssl", "/etc/resolv.conf", "/etc/hosts",
            "/etc/nsswitch.conf"})
        if (std::filesystem::exists(runtime))
          isolation.insert(isolation.end(), {"--ro-bind", runtime, runtime});
      for (const auto& runtime : runtime_store)
        isolation.insert(isolation.end(), {"--ro-bind", runtime, runtime});
      for (const auto& [source, target] : runtime_aliases)
        isolation.insert(isolation.end(), {"--ro-bind", source, target});
    }
    if (hide_credentials) {
      // Only what exists: there is nothing to hide in a missing directory,
      // and bubblewrap cannot create one under a read-only root.
      for (const auto& shared :
           std::set<std::string>{"/tmp", "/var/tmp", shared_tmp})
        if (std::filesystem::is_directory(shared))
          isolation.insert(isolation.end(), {"--tmpfs", shared});
    }
    for (const auto& path : hidden)
      isolation.insert(isolation.end(), {"--tmpfs", path});
    if (!view.hidden_root.empty())
      isolation.insert(isolation.end(), {"--tmpfs", view.hidden_root.string()});
    isolation.insert(isolation.end(),
                     {read_only ? "--ro-bind" : "--bind", root, root});
    for (const auto& path : view.writable)
      isolation.insert(isolation.end(),
                       {"--bind", path.string(), path.string()});
    for (const auto& mount : view.mounts)
      isolation.insert(isolation.end(),
                       {mount.writable ? "--bind" : "--ro-bind",
                        mount.source.string(), mount.target.string()});
    for (const auto& path : view.protected_files)
      isolation.insert(isolation.end(),
                       {"--ro-bind", path.string(), path.string()});
    for (const auto& path : view.excluded) {
      const auto under = [](const auto& parent, const auto& child) {
        const auto relative = child.lexically_relative(parent);
        return !relative.empty() && *relative.begin() != "..";
      };
      bool exposed =
          view.hidden_root.empty() || under(std::filesystem::path(root), path);
      for (const auto& mount : view.mounts)
        exposed = exposed || under(mount.target, path);
      for (const auto& runtime : runtime_store)
        exposed = exposed || under(std::filesystem::path(runtime), path) ||
                  under(path, std::filesystem::path(runtime));
      auto native = path;
      for (const auto& mount : view.mounts)
        if (under(mount.target, path))
          native = mount.source / path.lexically_relative(mount.target);
      if (!exposed || !std::filesystem::exists(native)) continue;
      if (std::filesystem::is_directory(native))
        isolation.insert(isolation.end(), {"--tmpfs", path.string(),
                                           "--remount-ro", path.string()});
      else
        isolation.insert(isolation.end(),
                         {"--ro-bind", "/dev/null", path.string()});
    }
    // A control directory inside the project must be covered after root.
    if (hide_credentials && !control.empty())
      isolation.insert(
          isolation.end(),
          {"--tmpfs", std::filesystem::weakly_canonical(control).string()});
    // Only this command's scratch is writable when read_only is selected.
    // Preserve shared temporary-file reads unless hide_credentials is selected.
    isolation.insert(isolation.end(), {"--bind", pattern, pattern});
    isolation.insert(isolation.end(), {"--proc", "/proc", "--dev", "/dev"});
    if (no_project_socket)
      isolation.insert(isolation.end(), {"--seccomp", "3"});
    isolation.push_back("--");
    spec.argv.insert(spec.argv.begin(), isolation.begin(), isolation.end());
#endif
  }
  spec.cwd = cwd;
  spec.stdout_fd = out.write;
  spec.stderr_fd = err.write;
  // The VM launcher supplies the project's environment. Never forward model
  // credentials or arbitrary host environment variables to a command.
  spec.env = {std::string("PATH=") +
              (std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin")};
  for (const char* key : {"USER", "LANG", "NIX_SSL_CERT_FILE", "SSL_CERT_FILE",
                          "TSUGUMI_PROJECT_ID"})
    if (const char* value = std::getenv(key))
      spec.env.push_back(std::string(key) + "=" + value);
  if (hide_credentials || read_only) {
    const auto scratch =
        temporary.path.empty() ? "/tmp" : temporary.path.string();
    spec.env.push_back("HOME=" + scratch);
    spec.env.push_back("TMPDIR=" + scratch);
  } else {
    for (const char* key : {"HOME", "TMPDIR", "NIX_REMOTE"})
      if (const char* value = std::getenv(key))
        spec.env.push_back(std::string(key) + "=" + value);
  }
  std::array<std::ofstream, 2> logs;
  if (!view.output_prefix.empty()) {
    for (size_t index = 0; index < 2; ++index) {
      logs[index].open(
          view.output_prefix.string() + (index ? ".stderr" : ".stdout"),
          std::ios::binary | std::ios::trunc);
      if (!logs[index])
        throw std::runtime_error("cannot preserve shell output");
    }
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
  Output result;
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
      // A command can ignore TERM and retain the sandbox after cancellation.
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
        const auto length = static_cast<size_t>(count);
        result.sizes[index] += length;
        if (logs[index].is_open()) {
          logs[index].write(buffer, count);
          if (!logs[index])
            throw std::runtime_error("cannot preserve shell output");
        }
        if (output.size() + length <= kOutputLimit)
          output.append(buffer, length);
        else {
          // Keep the beginning and the most recent end, even without newlines.
          const auto head = kOutputLimit / 2;
          std::string tail = output.substr(head);
          tail.append(buffer, length);
          if (tail.size() > head) tail.erase(0, tail.size() - head);
          output.resize(head);
          output += tail;
          result.truncated[index] = true;
        }
      } else if (errno != EAGAIN && errno != EINTR) {
        throw std::runtime_error("cannot read shell output");
      }
    }
  }
  return result;
}

namespace {

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
    std::lock_guard lock(cancellations_mu_);
    if (shell_stopping_) {
      fail(req, FailureData::make(FailureKind::cancelled,
                                  "the project is stopping"));
      return;
    }
    shell_cancellations_[key] = cancelled;
  }
  const bool accepted = shell_lane_->submit([this, req, cancelled,
                                             key]() mutable {
    try {
      if (cancelled->load())
        throw Failed(FailureKind::cancelled,
                     "the shell request was cancelled before starting");
      project_shell(req, *cancelled);
    } catch (const Failed& error) {
      fail(req, error.failure);
    } catch (const std::exception& error) {
      fail(req, FailureData::make(FailureKind::internal_error, error.what()));
    }
    std::lock_guard lock(cancellations_mu_);
    shell_cancellations_.erase(key);
  });
  if (!accepted) {
    std::lock_guard lock(cancellations_mu_);
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
  const auto* request = project_request(req)->value_as_ProjectShellRequest();
  const auto command = request->command()->str();
  // This short shell endpoint exposes a caller-controlled runtime budget.
  const auto timeout = request->timeout_ms().value_or(10000);
  if (timeout == 0 || timeout > 30000)
    throw Failed(FailureKind::invalid_request,
                 "timeout_ms must be between 1 and 30000");
  if (command.empty() || command.size() > 65536 ||
      command.find('\0') != std::string::npos)
    throw Failed(FailureKind::invalid_request,
                 "command must contain 1 to 65536 bytes without NUL");
  const auto operation = request->op_id()->str();
  const auto digest = request_digest(project_request(req));
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
    auto cwd = opts_.root;
    shell::View view;
    if (request->working_directory()) {
      const auto relative = request->working_directory()->str();
      if (!fs::valid_relative_path(relative) ||
          !fs::resolves_inside(opts_.root, opts_.root / relative))
        throw Failed(FailureKind::invalid_path, "invalid shell workspace");
      cwd /= relative;
    }
    auto path_in_view = [&](const std::string& relative) {
      if (!fs::valid_relative_path(relative) ||
          !fs::resolves_inside(cwd, cwd / relative))
        throw Failed(FailureKind::invalid_path, "invalid shell view path");
      return cwd / relative;
    };
    if (request->workspace_only())
      view.hidden_root = std::filesystem::canonical(opts_.root);
    if (request->writable_paths())
      for (const auto* path : *request->writable_paths())
        view.writable.push_back(path_in_view(path->str()));
    if (request->protected_paths())
      for (const auto* path : *request->protected_paths())
        view.protected_files.push_back(path_in_view(path->str()));
    if (request->output_prefix()) {
      const auto path = request->output_prefix()->str();
      if (!fs::valid_relative_path(path) ||
          !fs::resolves_inside(opts_.root, opts_.root / path))
        throw Failed(FailureKind::invalid_path, "invalid shell output path");
      view.output_prefix = opts_.root / path;
    }
    struct SymlinkCleanup {
      std::vector<std::filesystem::path> links;
      ~SymlinkCleanup() {
        std::error_code ec;
        for (const auto& link : links) std::filesystem::remove(link, ec);
      }
    } symlink_cleanup;
    if (request->mounts())
      for (const auto* mount : *request->mounts()) {
        auto source = mount->project_relative()
                          ? opts_.root / mount->path()->str()
                          : std::filesystem::path(mount->path()->str());
        source = std::filesystem::canonical(source);
        const auto target = path_in_view(mount->name()->str());
#ifdef __APPLE__
        std::filesystem::remove(target);
        std::filesystem::create_directory_symlink(source, target);
        symlink_cleanup.links.push_back(target);
#endif
        view.mounts.push_back({source, target, mount->writable()});
      }
    if (request->excluded_paths())
      for (const auto* path : *request->excluded_paths()) {
        const std::filesystem::path denied(path->str());
        view.excluded.push_back(std::filesystem::weakly_canonical(
            denied.is_absolute() ? denied : cwd / denied));
      }
    const auto output =
        shell::Execute(command, cwd.string(), timeout, cancelled,
                       request->read_only(), request->hide_credentials(),
                       request->no_project_socket(), opts_.control_dir, view);
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
        output.truncated[1], output.sizes[0], output.sizes[1],
        request->output_prefix()
            ? builder.CreateString(request->output_prefix()->str())
            : 0);
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
