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

#include "host/binary_caches.hh"
#include "tsugumi/util/crypto.hh"

namespace tsugumi::host {
namespace V = Tsugumi::VMHost;
namespace H = Tsugumi::Host;
auto VMResetPolicy() -> V::ResetT {
  V::ResetT request;
  request.discard_other_files = true;
  return request;
}
auto DescribeVMReset(H::HostRuntimeT& runtime) -> void {
  const auto policy = VMResetPolicy();
  runtime.reset_confirmation_ja =
      "開いているプロジェクトの実行と VM を停止します。";
  runtime.reset_confirmation_en = "Stop running projects and the VM. ";
  if (policy.discard_other_files) {
    runtime.reset_confirmation_ja +=
        "VM フォルダのすべて（共有 Nix store、展開した VM "
        "イメージ、ログなど）を削除します。";
    runtime.reset_confirmation_en +=
        "Delete everything in the VM folder (the shared Nix store, "
        "extracted VM image, logs and other files). ";
  }
  runtime.reset_confirmation_ja +=
      "各プロジェクトの実行環境フォルダ（control "
      "フォルダなど）も削除します。プロジェクトの文書・成果物・保存した実行履歴"
      "（.tsugumi/runs.sqlite）とホストの /nix/store "
      "は保持します。削除した実行環境は次に開くときに作り直します。";
  runtime.reset_confirmation_en +=
      "Delete each project's environment folder (including its control folder) "
      "as well. Keep project documents, artifacts, saved run history "
      "(.tsugumi/runs.sqlite) and the host's /nix/store. Recreate deleted "
      "environments when next opened.";
}
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
constexpr uint32_t kMaxVmFrameBytes = 64u * 1024u * 1024u;
auto Read(int fd) -> std::unique_ptr<V::MessageT> {
  unsigned char prefix[4];
  Transfer(fd, reinterpret_cast<char*>(prefix), 4, false);
  const uint32_t size = uint32_t(prefix[0]) | uint32_t(prefix[1]) << 8 |
                        uint32_t(prefix[2]) << 16 | uint32_t(prefix[3]) << 24;
  if (size > kMaxVmFrameBytes)
    throw std::runtime_error("VM helper message exceeds size limit");
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
    const auto* root = std::getenv("TSUGUMI_SOCKET_DIR");
    const auto prefix =
        root ? std::string("v-")
             : "tsugumi-vmhost-" + std::to_string(::getuid()) + "-";
    path_ = (std::filesystem::path(root ? root : "/tmp") /
             (prefix +
              crypto::sha256_hex(
                  std::filesystem::canonical(options_.support).string())
                  .substr(0, 24) +
              ".sock"))
                .string();
  }
  ~VMProcess() override { Shutdown(); }
  auto Shutdown() -> void override {
    if (stopping_.exchange(true)) return;
    startup_stop_.request_stop();
    // Do not launch a helper merely to stop it. Wait for guest poweroff
    // acknowledged by the resident helper.
    if (Socket socket{TryConnect()}; socket.fd >= 0) {
      try {
        H::HostRuntimeRequestT stop;
        stop.action = H::RuntimeAction::Shutdown;
        Send(socket.fd, stop);
        Read(socket.fd);
      } catch (...) {
      }
    }
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
    // VM boot is shared. Cancelling this caller releases its wait and never
    // sends the project start command; other callers can still use the VM.
    Call(Boot(launch.key), launch.stop);
    if (launch.stop.stop_requested())
      throw std::runtime_error("Environment start cancelled");
    V::RunT run;
    run.key = launch.key;
    auto name = std::filesystem::path(launch.project).filename().string();
    for (auto& ch : name)
      if (!std::isalnum(static_cast<unsigned char>(ch)) ||
          static_cast<unsigned char>(ch) > 127)
        ch = '-';
    run.host_name = name.empty() ? "project" : name.substr(0, 40);
    run.origins = launch.origins;
    run.mounts = Mounts(launch);
    auto reply = Call(std::move(run));
    const auto* process = reply->value.AsProcess();
    if (!process) throw std::runtime_error("Unexpected VM helper reply");
    Remember(process->pid, launch.key);
    return process->pid;
  }
  auto Inspect(H::EnvironmentStateT& ready) -> std::optional<int64_t> override {
    V::ProcessRequestT request;
    request.key = ready.key;
    const auto process = CallProcess(std::move(request));
    if (!process.running) return std::nullopt;
    ready.endpoint = process.endpoint;
    ready.unix_socket.clear();
    Remember(process.pid, ready.key);
    return process.pid;
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
      const auto process = CallProcess(std::move(request));
      return process.running && process.pid == pid ? std::nullopt
                                                   : std::optional<int>(-1);
    } catch (...) {
      // A transport failure does not prove a guest process has stopped.
      return std::nullopt;
    }
  }
  // Signal waits for the guest’s stop acknowledgement and process exit.
  auto WaitForExit(int64_t, bool) -> void override {}
  auto Signal(int64_t pid, int) -> void override {
    V::ProcessRequestT request;
    request.key = Key(pid);
    request.action = V::ProcessAction::Stop;
    Call(std::move(request));
  }
  auto GuardStart(const std::string& key) -> void override {
    V::ProcessRequestT request;
    request.key = key;
    if (CallProcess(std::move(request)).running)
      throw EnvironmentFailure(
          "A resident project process cannot be authenticated",
          "Stop the environment and retry.", "");
  }
  auto Runtime(const H::HostRuntimeRequestT& request)
      -> H::HostRuntimeT override {
    if (request.action == H::RuntimeAction::CleanupStore ||
        request.action == H::RuntimeAction::VerifyStore)
      Call(Boot(""));
    auto reply = request.action == H::RuntimeAction::Reset
                     ? Call(VMResetPolicy())
                     : Call(request);
    const auto* runtime = reply->value.AsHostRuntime();
    if (!runtime) throw std::runtime_error("Unexpected VM helper runtime reply");
    return *runtime;
  }
  auto Terminal(const H::HostTerminalRequestT& request)
      -> H::HostTerminalT override {
    auto reply = Call(request);
    const auto* terminal = reply->value.AsHostTerminal();
    if (!terminal)
      throw std::runtime_error("Unexpected VM helper terminal reply");
    return *terminal;
  }

 private:
  VMProcessOptions options_;
  std::string path_;
  std::mutex open_mutex_, watch_mutex_, keys_mutex_;
  std::atomic<int> watch_fd_ = -1;
  std::stop_source startup_stop_;
  std::atomic<bool> stopping_ = false;
  std::mutex calls_mutex_;
  std::set<int> calls_;
  int64_t helper_pid_ = 0;
  std::jthread watcher_;
  std::map<int64_t, std::string> keys_;
  auto Boot(const std::string& key) -> V::BootT {
    V::BootT boot;
    boot.key = key;
    boot.broker_socket = options_.broker_socket;
    const auto caches = PrepareBinaryCaches(options_.support);
    boot.cache_configuration = caches.configuration;
    boot.cache_netrc = caches.netrc;
    boot.cache_problem = caches.problem;
    boot.resources =
        std::make_unique<H::VMResourcesT>(options_.resources());
    return boot;
  }
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
    const auto fd = socket.fd;
    socket.fd = -1;
    return fd;
  }
  auto Connect() -> int {
    std::lock_guard lock(open_mutex_);
    if (auto fd = TryConnect(); fd >= 0) return fd;
    EnvironmentLaunch launch;
    launch.vm_helper = true;
    launch.arguments = {options_.binary.string(), "--support",
                        options_.support.string(), "--image",
                        options_.image.string()};
    launch.log = options_.support / "vmhost.log";
    auto native = NativeEnvironmentProcess();
    helper_pid_ = native->Launch(launch);
    int connected = -1;
    WaitForNativeReadiness(path_, helper_pid_, startup_stop_.get_token(), [&] {
      connected = TryConnect();
      if (connected >= 0) return true;
      if (native->Poll(helper_pid_, true).has_value())
        throw std::runtime_error(
            "VM helper exited during startup; see vmhost.log");
      return false;
    });
    if (connected < 0) throw std::runtime_error("VM helper start cancelled");
    return connected;
  }
  auto Watch() -> void {
    std::lock_guard lock(watch_mutex_);
    if (stopping_) throw std::runtime_error("VM adapter is stopping");
    if (watch_fd_ >= 0) return;
    if (watcher_.joinable()) watcher_.join();
    watch_fd_ = Connect();
    try {
      Send(watch_fd_, V::WatchT{});
      // The helper acknowledges Watch before subscribing this peer.
      Read(watch_fd_);
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
              if (!event) continue;
              if (const auto* progress = event->value.AsProgress())
                options_.progress(*progress);
              if (const auto* runtime = event->value.AsHostRuntime())
                options_.runtime(*runtime);
              if (const auto* terminal = event->value.AsHostTerminalData())
                options_.terminal(*terminal);
            }
          } catch (...) {
          }
          {
            std::lock_guard lock(watch_mutex_);
            // Watch is re-established by the next explicit operation.
            watch_fd_ = -1;
            ::close(fd);
          }
        });
  }
  template <class T>
  auto Call(T value, std::stop_token stop = {}) -> std::unique_ptr<V::ReplyT> {
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
    std::stop_callback cancelled(stop,
                                 [&] { ::shutdown(socket.fd, SHUT_RDWR); });
    Send(socket.fd, std::move(value));
    auto message = Read(socket.fd);
    auto* reply = message->payload.AsReply();
    if (!reply) throw std::runtime_error("Unexpected VM helper message");
    if (const auto* failure = reply->value.AsFailure())
      throw EnvironmentFailure(failure->message, failure->recovery,
                               failure->log);
    return std::make_unique<V::ReplyT>(*reply);
  }
  auto CallProcess(V::ProcessRequestT request) -> V::ProcessT {
    auto reply = Call(std::move(request));
    auto* process = reply->value.AsProcess();
    if (!process) throw std::runtime_error("Unexpected VM helper process reply");
    return std::move(*process);
  }
};
}  // namespace

auto VMEnvironmentProcess(VMProcessOptions options)
    -> std::shared_ptr<EnvironmentProcess> {
  return std::make_shared<VMProcess>(std::move(options));
}
}  // namespace tsugumi::host
