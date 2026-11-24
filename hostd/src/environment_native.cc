#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/inotify.h>
#include <sys/syscall.h>
#endif

#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "host/environment.hh"
#include "host/native_terminal.hh"
#include "host/vm_process.hh"
#include "tsugumi/client/client.hh"
#include "tsugumi/protocol/tsugumi_generated.h"
#include "tsugumi/rpc/trace.hh"
#include "tsugumi/util/crypto.hh"

namespace tsugumi::host {
namespace {
using namespace std::chrono_literals;
class NativeProcess final : public EnvironmentProcess {
  NativeTerminals terminals_;

 public:
  explicit NativeProcess(NativeTerminals::Output output)
      : terminals_(std::move(output)) {}
  auto Shutdown() -> void override { terminals_.Shutdown(); }
  auto Terminal(const Tsugumi::Host::HostTerminalRequestT& request)
      -> Tsugumi::Host::HostTerminalT override {
    return terminals_.Handle(request);
  }
  auto Launch(const EnvironmentLaunch& launch) -> int64_t override {
    std::vector<char*> arguments;
    for (const auto& argument : launch.arguments)
      arguments.push_back(const_cast<char*>(argument.c_str()));
    arguments.push_back(nullptr);
    // Project runtime values come from the OS and fixed installation paths.
    // The VM helper additionally receives only its named startup options.
    passwd user{};
    passwd* found = nullptr;
    std::vector<char> buffer(16384);
    if (::getpwuid_r(::getuid(), &user, buffer.data(), buffer.size(), &found) ||
        !found)
      throw std::runtime_error("Cannot determine the project process home");
    const std::string home = user.pw_dir;
    std::vector<std::string> environment = {
        "HOME=" + home, "TMPDIR=/tmp", "LANG=C.UTF-8",
        "PATH=" + home +
            "/.nix-profile/bin:/nix/var/nix/profiles/default/bin:"
            "/run/current-system/sw/bin:/usr/bin:/bin:/usr/sbin:/sbin"};
    // These are filesystem locations, not credentials. Preserve the host's
    // explicit XDG storage layout when starting a project or VM helper.
    for (const auto* name :
         {"XDG_CACHE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME"})
      if (const auto* value = std::getenv(name))
        environment.push_back(std::string(name) + "=" + value);
#ifdef __linux__
    if (!launch.vm_helper)
      for (const auto* name : {"DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR",
                               "TSUGUMI_CARD_CGROUP_ROOT"})
        if (const auto* value = std::getenv(name))
          environment.push_back(std::string(name) + "=" + value);
#endif
    if (trace::LaunchEnabled())
      environment.push_back(std::string(trace::kLaunchVariable) + "=1");
    if (launch.vm_helper) {
      for (const auto* name :
           {"TSUGUMI_VM_X86", "TSUGUMI_VM_CMDLINE", "TSUGUMI_PY_BINDINGS",
            "TSUGUMID", "TSUGUMI_WORKER", "TSUGUMI_NIX_DIR", "HOST_LOG",
            "HOST_BUSY", "TSUGUMI_SOCKET_DIR"}) {
        if (const auto* value = std::getenv(name))
          environment.push_back(std::string(name) + "=" + value);
      }
    }
    for (const auto* certificate :
         {"/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/cert.pem",
          "/nix/var/nix/profiles/default/etc/ssl/certs/ca-bundle.crt"}) {
      if (::access(certificate, R_OK) == 0) {
        environment.push_back(std::string("SSL_CERT_FILE=") + certificate);
        environment.push_back(std::string("NIX_SSL_CERT_FILE=") + certificate);
        break;
      }
    }
    if (launch.authorization.available) {
      environment.push_back("TSUGUMI_EXECUTION_BROKER=" +
                            launch.authorization.endpoint);
      environment.push_back("TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE=" +
                            launch.authorization.capability_file);
    }
    std::vector<char*> env;
    for (auto& value : environment) env.push_back(value.data());
    env.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, launch.log.c_str(),
                                     O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW,
                                     0600);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGCHLD);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGHUP);
    posix_spawnattr_setsigmask(&attributes, &empty);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setpgroup(&attributes, 0);
    short flags =
        POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attributes, flags);
    pid_t pid = -1;
    const auto error = posix_spawn(&pid, arguments.front(), &actions,
                                   &attributes, arguments.data(), env.data());
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (error)
      throw std::runtime_error("Cannot start tsugumid: " +
                               std::string(std::strerror(error)));
    return pid;
  }
  auto Poll(int64_t value, bool child) -> std::optional<int> override {
    const auto pid = static_cast<pid_t>(value);
    if (pid <= 0) return -1;
    if (child) {
      int status = 0;
      pid_t found;
      do {
        found = ::waitpid(pid, &status, WNOHANG);
      } while (found < 0 && errno == EINTR);
      if (found == pid) return status;
      if (found < 0 && errno == ECHILD) return -1;
    }
    return ::kill(pid, 0) == 0 ? std::nullopt : std::optional<int>(-1);
  }
  auto WaitForReady(const std::filesystem::path& file, const std::string&,
                    int64_t pid, std::stop_token stop,
                    const std::function<bool()>& done) -> void override {
    WaitForNativeReadiness(file, pid, stop, done);
  }
  auto WaitForExit(int64_t pid, bool child) -> void override {
    if (child) {
      while (::waitpid(static_cast<pid_t>(pid), nullptr, 0) < 0 &&
             errno == EINTR) {
      }
      return;
    }
    WaitForNativeReadiness({}, pid, {},
                           [&] { return Poll(pid, false).has_value(); });
  }
  auto Signal(int64_t pid, int signal) -> void override {
    if (pid > 0 && ::kill(static_cast<pid_t>(pid), signal) != 0 &&
        errno != ESRCH)
      throw std::runtime_error("Cannot signal tsugumid: " +
                               std::string(std::strerror(errno)));
  }
  auto Verify(const Tsugumi::Host::EnvironmentStateT& ready) -> bool override {
    try {
      return VerifyEnvironmentEndpoint(ready);
    } catch (const std::exception&) {
      return false;
    }
  }
};
}  // namespace

auto WaitForNativeReadiness(const std::filesystem::path& file, int64_t pid,
                            std::stop_token stop,
                            const std::function<bool()>& done) -> void {
  struct Descriptor {
    int fd;
    ~Descriptor() {
      if (fd >= 0) ::close(fd);
    }
  };
  int wake[2];
  if (::pipe(wake) != 0) throw std::runtime_error("Cannot open readiness pipe");
  Descriptor reader{wake[0]}, writer{wake[1]};
  for (const auto fd : wake) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  std::stop_callback cancelled(stop, [&] { (void)::write(writer.fd, "x", 1); });
#ifdef __APPLE__
  Descriptor events{::kqueue()};
  Descriptor directory{
      file.empty() ? -1
                   : ::open(file.parent_path().c_str(), O_EVTONLY | O_CLOEXEC)};
  struct kevent changes[3];
  int count = 0;
  if (directory.fd >= 0) {
    EV_SET(&changes[count], directory.fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, nullptr);
    ++count;
  }
  if (!file.empty() && directory.fd < 0)
    throw std::runtime_error("Cannot watch readiness directory");
  EV_SET(&changes[count], reader.fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
  ++count;
  EV_SET(&changes[count], pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0,
         nullptr);
  ++count;
  if (::kevent(events.fd, changes, count, nullptr, 0, nullptr) < 0) {
    if (errno == ESRCH && done()) return;
    throw std::runtime_error("Cannot watch environment readiness");
  }
  while (!stop.stop_requested() && !done()) {
    struct kevent event{};
    if (::kevent(events.fd, nullptr, 0, &event, 1, nullptr) < 0 &&
        errno != EINTR)
      throw std::runtime_error("Cannot wait for environment readiness");
    if (event.filter == EVFILT_PROC) {
      (void)done();
      return;
    }
  }
#else
  Descriptor process{static_cast<int>(::syscall(SYS_pidfd_open, pid, 0))};
  Descriptor events{file.empty() ? -1 : ::inotify_init1(IN_CLOEXEC)};
  if (!file.empty() &&
      (events.fd < 0 ||
       ::inotify_add_watch(events.fd, file.parent_path().c_str(),
                           IN_CREATE | IN_MOVED_TO | IN_CLOSE_WRITE |
                               IN_DELETE_SELF | IN_MOVE_SELF) < 0))
    throw std::runtime_error("Cannot watch readiness directory");
  if (process.fd < 0) {
    if (errno == ESRCH && done()) return;
    throw std::runtime_error("Cannot watch environment process");
  }
  pollfd fds[] = {
      {reader.fd, POLLIN, 0}, {process.fd, POLLIN, 0}, {events.fd, POLLIN, 0}};
  while (!stop.stop_requested() && !done()) {
    const auto ready = ::poll(fds, 3, -1);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0)
      throw std::runtime_error("Cannot wait for environment readiness");
    if (fds[1].revents) {
      (void)done();
      return;
    }
    if (fds[2].revents & POLLIN) {
      char buffer[4096];
      if (::read(events.fd, buffer, sizeof buffer) <= 0)
        throw std::runtime_error("Cannot read environment readiness event");
    }
  }
#endif
}

namespace {
auto ProjectConnection(const Tsugumi::Host::EnvironmentStateT& ready)
    -> std::unique_ptr<client::Connection> {
  namespace R = Tsugumi::RPC;
  client::Options options;
  options.endpoint =
      ready.unix_socket.empty() ? ready.endpoint : "unix:" + ready.unix_socket;
  auto connection = std::make_unique<client::Connection>(options);
  if (connection->Start().get())
    throw std::runtime_error("Cannot connect to project core");
  auto response =
      connection
          ->Call([&](uint64_t id) {
            flatbuffers::FlatBufferBuilder b;
            auto hello = R::CreateSessionHelloRequest(
                b, b.CreateString(ready.project_id),
                b.CreateString(ready.token), Tsugumi::Trace::Client::Host);
            auto request = R::CreateRequest(
                b, R::RequestPayload::SessionHelloRequest, hello.Union());
            R::FinishMessageBuffer(
                b,
                R::CreateMessage(b, id, R::Payload::Request, request.Union()));
            return client::Bytes(b.GetBufferPointer(),
                                 b.GetBufferPointer() + b.GetSize());
          })
          .get();
  if (response.error) throw std::runtime_error("Project handshake failed");
  return connection;
}
auto ReadProjectRequirements(const Tsugumi::Host::EnvironmentStateT& ready)
    -> Tsugumi::RPC::ProjectHostRequirementsT {
  namespace R = Tsugumi::RPC;
  auto connection = ProjectConnection(ready);
  auto result = connection
                    ->Call([](uint64_t id) {
                      flatbuffers::FlatBufferBuilder b;
                      auto value = R::CreateProjectHostRequirementsRequest(b);
                      auto request = R::CreateRequest(
                          b, R::RequestPayload::ProjectHostRequirementsRequest,
                          value.Union());
                      R::FinishMessageBuffer(
                          b, R::CreateMessage(b, id, R::Payload::Request,
                                              request.Union()));
                      return client::Bytes(b.GetBufferPointer(),
                                           b.GetBufferPointer() + b.GetSize());
                    })
                    .get();
  if (result.error)
    throw std::runtime_error("Cannot read project host requirements");
  const auto* value = R::GetMessage(result.bytes.data())->payload_as_Reply();
  const auto* requirements =
      value ? value->value_as_ProjectHostRequirements() : nullptr;
  if (!requirements)
    throw std::runtime_error("Missing project host requirements");
  Tsugumi::RPC::ProjectHostRequirementsT out;
  requirements->UnPackTo(&out);
  return out;
}
}  // namespace
auto EnvironmentProcess::Busy(const Tsugumi::Host::EnvironmentStateT& ready)
    -> bool {
  namespace R = Tsugumi::RPC;
  auto connection = ProjectConnection(ready);
  auto result =
      connection
          ->Call([](uint64_t id) {
            flatbuffers::FlatBufferBuilder b;
            auto value = R::CreateProjectActivityRequest(b);
            auto request = R::CreateRequest(
                b, R::RequestPayload::ProjectActivityRequest, value.Union());
            R::FinishMessageBuffer(
                b,
                R::CreateMessage(b, id, R::Payload::Request, request.Union()));
            return client::Bytes(b.GetBufferPointer(),
                                 b.GetBufferPointer() + b.GetSize());
          })
          .get();
  if (result.error) throw std::runtime_error("Cannot read project activity");
  const auto* reply = R::GetMessage(result.bytes.data())->payload_as_Reply();
  const auto* activity = reply ? reply->value_as_ProjectActivity() : nullptr;
  if (!activity) throw std::runtime_error("Missing project activity");
  return activity->busy();
}
auto EnvironmentProcess::Requirements(
    const Tsugumi::Host::EnvironmentStateT& ready)
    -> Tsugumi::RPC::ProjectHostRequirementsT {
  return ReadProjectRequirements(ready);
}
auto VerifyEnvironmentEndpoint(const Tsugumi::Host::EnvironmentStateT& ready)
    -> bool {
  try {
    ProjectConnection(ready);
    return true;
  } catch (...) {
    return false;
  }
}
auto NativeEnvironmentProcess(
    std::function<void(const Tsugumi::Host::HostTerminalDataT&)> terminal)
    -> std::shared_ptr<EnvironmentProcess> {
  return std::make_shared<NativeProcess>(std::move(terminal));
}
}  // namespace tsugumi::host
