#include "host/vm_process.hh"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "util/crypto.hh"

namespace tsugumi::host {
namespace V = Tsugumi::VMHost;
namespace H = Tsugumi::Host;
namespace {
using namespace std::chrono_literals;
struct Socket {
  int fd;
  ~Socket() {
    if (fd >= 0) ::close(fd);
  }
};
auto Transfer(int fd, char* bytes, size_t size, bool send) -> void {
  size_t offset = 0;
  while (offset < size) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const auto n = send ? ::send(fd, bytes + offset, size - offset, flags)
                        : ::read(fd, bytes + offset, size - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0)
      throw EnvironmentFailure(
          "The VM helper connection closed",
          "Retry the connection. Running processes continue.", "");
    offset += static_cast<size_t>(n);
  }
}
auto Read(int fd) -> std::unique_ptr<V::MessageT> {
  unsigned char prefix[4];
  Transfer(fd, reinterpret_cast<char*>(prefix), 4, false);
  const uint32_t size = uint32_t(prefix[0]) | uint32_t(prefix[1]) << 8 |
                        uint32_t(prefix[2]) << 16 | uint32_t(prefix[3]) << 24;
  if (!size || size > 16u << 20)
    throw std::runtime_error("Invalid VM helper frame");
  std::string bytes(size, '\0');
  Transfer(fd, bytes.data(), bytes.size(), false);
  flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(bytes.data()),
                                 bytes.size());
  if (!V::VerifyMessageBuffer(verifier))
    throw std::runtime_error("Invalid VM helper message");
  return std::unique_ptr<V::MessageT>(V::GetMessage(bytes.data())->UnPack());
}
template <class T>
auto Send(int fd, T value) -> void {
  V::RequestT request;
  request.value.Set(std::move(value));
  V::MessageT message;
  message.request_id = 1;
  message.payload.Set(std::move(request));
  flatbuffers::FlatBufferBuilder builder;
  V::FinishMessageBuffer(builder, V::Message::Pack(builder, &message));
  const auto size = static_cast<uint32_t>(builder.GetSize());
  char prefix[4] = {static_cast<char>(size), static_cast<char>(size >> 8),
                    static_cast<char>(size >> 16),
                    static_cast<char>(size >> 24)};
  Transfer(fd, prefix, 4, true);
  Transfer(fd, reinterpret_cast<char*>(builder.GetBufferPointer()),
           builder.GetSize(), true);
}
class VMProcess final : public EnvironmentProcess {
 public:
  explicit VMProcess(VMProcessOptions options) : options_(std::move(options)) {
    std::filesystem::create_directories(options_.support);
    path_ = "/tmp/tsugumi-vmhost-" + std::to_string(::getuid()) + "-" +
            crypto::sha256_hex(
                std::filesystem::canonical(options_.support).string())
                .substr(0, 24) +
            ".sock";
  }
  ~VMProcess() override { Shutdown(); }
  auto Shutdown() -> void override {
    if (stopping_.exchange(true)) return;
    watcher_.request_stop();
    {
      std::lock_guard lock(watch_mutex_);
      if (watch_fd_ >= 0) ::shutdown(watch_fd_, SHUT_RDWR);
    }
    {
      std::lock_guard lock(calls_mutex_);
      for (const auto fd : calls_) ::shutdown(fd, SHUT_RDWR);
    }
    if (watcher_.joinable()) watcher_.join();
  }
  auto Guest() const -> bool override { return true; }
  auto Launch(const EnvironmentLaunch& launch) -> int64_t override {
    V::BootT boot;
    boot.key = launch.key;
    boot.broker_endpoint = options_.broker_endpoint;
    Call(std::move(boot));
    V::RunT run;
    run.key = launch.key;
    run.cpus = options_.cpus;
    auto name = std::filesystem::path(launch.project).filename().string();
    for (auto& ch : name)
      if (!std::isalnum(static_cast<unsigned char>(ch)) ||
          static_cast<unsigned char>(ch) > 127)
        ch = '-';
    run.host_name = name.empty() ? "project" : name.substr(0, 40);
    run.origins = launch.origins;
    run.archive_incompatible_history = launch.archive;
    run.mounts = Mounts(launch);
    auto reply = Call(std::move(run));
    const auto* process = reply->value.AsProcess();
    if (!process || !process->running || process->pid <= 0)
      throw std::runtime_error("VM helper returned no process");
    Remember(process->pid, launch.key);
    return process->pid;
  }
  auto Inspect(H::EnvironmentStateT& ready) -> std::optional<int64_t> override {
    V::ProcessRequestT request;
    request.key = ready.key;
    const auto reply = Call(std::move(request));
    const auto* process = reply->value.AsProcess();
    if (!process || !process->running || process->pid <= 0) return std::nullopt;
    ready.endpoint = process->endpoint;
    ready.unix_socket.clear();
    if (!VerifyEnvironmentEndpoint(ready)) return std::nullopt;
    Remember(process->pid, ready.key);
    return process->pid;
  }
  auto Verify(const H::EnvironmentStateT& ready) -> bool override {
    auto copy = ready;
    try {
      return Inspect(copy).has_value();
    } catch (...) {
      return false;
    }
  }
  auto Poll(int64_t pid, bool) -> std::optional<int> override {
    V::ProcessRequestT request;
    request.key = Key(pid);
    try {
      const auto reply = Call(std::move(request));
      const auto* process = reply->value.AsProcess();
      return process && process->running && process->pid == pid
                 ? std::nullopt
                 : std::optional<int>(-1);
    } catch (...) {
      // A transport failure does not prove a guest process has stopped.
      return std::nullopt;
    }
  }
  auto Signal(int64_t pid, int) -> void override {
    V::ProcessRequestT request;
    request.key = Key(pid);
    request.action = V::ProcessAction::Stop;
    Call(std::move(request));
  }
  auto GuardStart(const std::string& key) -> void override {
    V::ProcessRequestT request;
    request.key = key;
    const auto reply = Call(std::move(request));
    if (const auto* process = reply->value.AsProcess();
        process && process->running)
      throw EnvironmentFailure(
          "A resident project process cannot be authenticated",
          "Stop the environment and retry.", "");
  }
  auto Shares(const EnvironmentLaunch& launch) -> void override {
    V::SharesT shares;
    shares.key = launch.key;
    shares.mounts = Mounts(launch);
    Call(std::move(shares));
  }
  auto Runtime(const H::HostRuntimeRequestT& request)
      -> H::HostRuntimeT override {
    if (request.action == H::RuntimeAction::CleanupStore ||
        request.action == H::RuntimeAction::Histories) {
      V::BootT boot;
      boot.broker_endpoint = options_.broker_endpoint;
      Call(std::move(boot));
    }
    auto reply = Call(request);
    const auto* runtime = reply->value.AsHostRuntime();
    if (!runtime)
      throw std::runtime_error("VM helper returned no runtime state");
    return *runtime;
  }
  auto Terminal(const H::HostTerminalRequestT& request)
      -> H::HostTerminalT override {
    auto reply = Call(request);
    const auto* terminal = reply->value.AsHostTerminal();
    if (!terminal)
      throw std::runtime_error("VM helper returned no terminal result");
    return *terminal;
  }

 private:
  VMProcessOptions options_;
  std::string path_;
  std::mutex open_mutex_, watch_mutex_, keys_mutex_;
  std::atomic<int> watch_fd_ = -1;
  std::atomic<bool> stopping_ = false;
  std::mutex calls_mutex_;
  std::set<int> calls_;
  int64_t helper_pid_ = 0;
  std::jthread watcher_;
  std::map<int64_t, std::string> keys_;
  auto Remember(int64_t pid, const std::string& key) -> void {
    std::lock_guard lock(keys_mutex_);
    keys_[pid] = key;
  }
  auto Key(int64_t pid) -> std::string {
    std::lock_guard lock(keys_mutex_);
    return keys_.at(pid);
  }
  auto Mounts(const EnvironmentLaunch& launch)
      -> std::vector<std::unique_ptr<V::MountT>> {
    std::vector<std::unique_ptr<V::MountT>> mounts;
    for (const auto& mount : launch.mounts) {
      auto value = std::make_unique<V::MountT>();
      value->name = mount.name;
      value->path = mount.path;
      value->read_only = mount.read_only;
      mounts.push_back(std::move(value));
    }
    return mounts;
  }
  auto TryConnect() -> int {
    struct stat st{};
    if (::lstat(path_.c_str(), &st) || !S_ISSOCK(st.st_mode) ||
        st.st_uid != ::getuid() || (st.st_mode & 0077))
      return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(address.sun_path))
      throw std::runtime_error("VM helper socket path too long");
    std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
    Socket socket{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (socket.fd < 0) return -1;
    ::fcntl(socket.fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    ::setsockopt(socket.fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
    if (::connect(socket.fd, reinterpret_cast<sockaddr*>(&address),
                  sizeof address))
      return -1;
    timeval timeout{1800, 0};
    ::setsockopt(socket.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    timeout.tv_sec = 5;
    ::setsockopt(socket.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    const auto fd = socket.fd;
    socket.fd = -1;
    return fd;
  }
  auto Connect() -> int {
    std::lock_guard lock(open_mutex_);
    if (auto fd = TryConnect(); fd >= 0) return fd;
    EnvironmentLaunch launch;
    launch.vm_helper = true;
    launch.arguments = {options_.binary.string(),        "--support",
                        options_.support.string(),       "--image",
                        options_.image.string(),         "--source",
                        options_.source.string(),        "--cpus",
                        std::to_string(options_.cpus),   "--memory",
                        std::to_string(options_.memory), "--store",
                        std::to_string(options_.store)};
    for (const auto& entry : options_.source_entries) {
      launch.arguments.push_back("--source-entry");
      launch.arguments.push_back(entry);
    }
    launch.log = options_.support / "vmhost.log";
    auto native = NativeEnvironmentProcess();
    helper_pid_ = native->Launch(launch);
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (auto fd = TryConnect(); fd >= 0) return fd;
      if (native->Poll(helper_pid_, true).has_value())
        throw std::runtime_error(
            "VM helper exited during startup; see vmhost.log");
      std::this_thread::sleep_for(50ms);
    }
    throw std::runtime_error("VM helper did not become ready; see vmhost.log");
  }
  auto Watch() -> void {
    std::lock_guard lock(watch_mutex_);
    if (stopping_) throw std::runtime_error("VM adapter is stopping");
    if (watch_fd_ >= 0) return;
    if (watcher_.joinable()) watcher_.join();
    watch_fd_ = Connect();
    try {
      Send(watch_fd_, V::WatchT{});
      auto message = Read(watch_fd_);
      if (!message->payload.AsReply())
        throw std::runtime_error("VM helper did not acknowledge Watch");
    } catch (...) {
      ::close(watch_fd_);
      watch_fd_ = -1;
      throw;
    }
    watcher_ =
        std::jthread([this, fd = watch_fd_.load()](std::stop_token stop) {
          try {
            while (!stop.stop_requested()) {
              auto message = Read(fd);
              const auto* event = message->payload.AsEvent();
              if (!event) throw std::runtime_error("Expected VM helper event");
              if (const auto* progress = event->value.AsProgress())
                options_.progress(*progress);
              if (const auto* runtime = event->value.AsHostRuntime())
                options_.runtime(*runtime);
              if (const auto* terminal = event->value.AsHostTerminalData())
                options_.terminal(*terminal);
            }
          } catch (...) {
          }
          ::close(fd);
          // Watch is re-established by the next explicit operation.
          watch_fd_ = -1;
        });
  }
  template <class T>
  auto Call(T value) -> std::unique_ptr<V::ReplyT> {
    Watch();
    Socket socket{Connect()};
    {
      std::lock_guard lock(calls_mutex_);
      if (stopping_) throw std::runtime_error("VM adapter is stopping");
      calls_.insert(socket.fd);
    }
    struct Finished {
      VMProcess& self;
      int fd;
      ~Finished() {
        std::lock_guard lock(self.calls_mutex_);
        self.calls_.erase(fd);
      }
    } finished{*this, socket.fd};
    Send(socket.fd, std::move(value));
    auto message = Read(socket.fd);
    auto* reply = message->payload.AsReply();
    if (!reply || message->request_id != 1)
      throw std::runtime_error("Invalid VM helper reply");
    if (const auto* failure = reply->value.AsFailure())
      throw EnvironmentFailure(failure->message, failure->recovery,
                               failure->log);
    return std::make_unique<V::ReplyT>(*reply);
  }
};
}  // namespace

auto VMEnvironmentProcess(VMProcessOptions options)
    -> std::shared_ptr<EnvironmentProcess> {
  return std::make_shared<VMProcess>(std::move(options));
}
}  // namespace tsugumi::host
