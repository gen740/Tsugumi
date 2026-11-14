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

#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "host/environment.hh"
#include "host/vm_process.hh"
#include "tsugumi/client/client.hh"
#include "tsugumi/protocol/tsugumi_generated.h"
#include "util/crypto.hh"

namespace tsugumi::host {
namespace {
using namespace std::chrono_literals;
class NativeProcess final : public EnvironmentProcess {
 public:
  auto Launch(const EnvironmentLaunch& launch) -> int64_t override {
    std::vector<char *> arguments;
    for (const auto &argument : launch.arguments)
      arguments.push_back(const_cast<char *>(argument.c_str()));
    arguments.push_back(nullptr);
    // Project runtime values come from the OS and fixed installation paths.
    // The VM helper additionally receives only its named startup options.
    passwd user{};
    passwd *found = nullptr;
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
    if (launch.vm_helper) {
      for (const auto *name :
           {"TSUGUMI_VM_X86", "TSUGUMI_VM_CMDLINE", "TSUGUMI_PY_BINDINGS",
            "TSUGUMID", "TSUGUMI_WORKER", "TSUGUMI_SDK_DIR", "HOST_LOG",
            "HOST_BUSY", "HOST_PID"}) {
        if (const auto *value = std::getenv(name))
          environment.push_back(std::string(name) + "=" + value);
      }
    }
    for (const auto *certificate :
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
    std::vector<char *> env;
    for (auto &value : environment)
      env.push_back(value.data());
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
  auto Signal(int64_t pid, int signal) -> void override {
    if (pid > 0 && ::kill(static_cast<pid_t>(pid), signal) != 0 &&
        errno != ESRCH)
      throw std::runtime_error("Cannot signal tsugumid: " +
                               std::string(std::strerror(errno)));
  }
  auto Verify(const Tsugumi::Host::EnvironmentStateT& ready) -> bool override {
    try {
      // A stale readiness file must never make an unrelated reused PID ours.
      struct stat metadata{};
      if (::lstat(ready.unix_socket.c_str(), &metadata) ||
          !S_ISSOCK(metadata.st_mode) || metadata.st_uid != ::getuid() ||
          (metadata.st_mode & 0077))
        return false;
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      if (ready.unix_socket.size() >= sizeof(address.sun_path)) return false;
      std::memcpy(address.sun_path, ready.unix_socket.c_str(),
                  ready.unix_socket.size() + 1);
      const auto fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0) return false;
      struct Close {
        int fd;
        ~Close() { ::close(fd); }
      } close{fd};
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
      ::fcntl(fd, F_SETFL, O_NONBLOCK);
      if (::connect(fd, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) &&
          errno != EINPROGRESS)
        return false;
      pollfd poll{fd, POLLOUT, 0};
      if (::poll(&poll, 1, 500) <= 0) return false;
#if defined(__linux__)
      ucred peer{};
      socklen_t size = sizeof(peer);
      if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) ||
          peer.pid != ready.pid || peer.uid != ::getuid())
        return false;
#elif defined(LOCAL_PEERPID)
      pid_t peer = -1;
      socklen_t size = sizeof(peer);
      if (::getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &peer, &size) ||
          peer != ready.pid)
        return false;
#endif
      return VerifyEnvironmentEndpoint(ready);
    } catch (const std::exception&) {
      return false;
    }
  }
};
}  // namespace

auto VerifyEnvironmentEndpoint(const Tsugumi::Host::EnvironmentStateT& ready)
    -> bool {
  namespace R = Tsugumi::RPC;
  try {
    client::Options options;
    options.endpoint = ready.unix_socket.empty() ? ready.endpoint
                                                 : "unix:" + ready.unix_socket;
    options.connect_timeout = 500ms;
    client::Connection connection(options);
    if (connection.Start().get()) return false;
    auto response =
        connection
            .Call(
                [&](uint64_t id) {
                  flatbuffers::FlatBufferBuilder b;
                  auto hello = R::CreateSessionHelloRequest(
                      b, 1, b.CreateString(crypto::uuid4()),
                      b.CreateString(ready.project_id),
                      b.CreateString(ready.token));
                  auto request = R::CreateRequest(
                      b, R::RequestPayload::SessionHelloRequest, hello.Union());
                  R::FinishMessageBuffer(
                      b, R::CreateMessage(b, id, R::Payload::Request,
                                          request.Union()));
                  return client::Bytes(b.GetBufferPointer(),
                                       b.GetBufferPointer() + b.GetSize());
                },
                500ms)
            .get();
    if (response.error) return false;
    const auto* reply =
        R::GetMessage(response.bytes.data())->payload_as_Reply();
    const auto* hello = reply ? reply->value_as_HelloResult() : nullptr;
    return hello && hello->project_id()->str() == ready.project_id &&
           hello->server_instance_id()->str() == ready.server_instance_id;
  } catch (...) {
    return false;
  }
}
auto NativeEnvironmentProcess() -> std::shared_ptr<EnvironmentProcess> {
  return std::make_shared<NativeProcess>();
}
}  // namespace tsugumi::host
