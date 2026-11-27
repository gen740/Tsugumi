#include "host/vm_process.hh"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "host/binary_caches.hh"
#include "tsugumi/protocol/trace_codec.hh"
#include "tsugumi/rpc/frame.hh"
#include "tsugumi/rpc/transport.hh"
#include "tsugumi/rpc/unix_socket.hh"
#include "tsugumi/util/crypto.hh"

namespace tsugumi::host {
namespace V = Tsugumi::VMHost;
namespace H = Tsugumi::Host;
namespace G = Tsugumi::VMGuest;
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
struct Socket {
  int fd;
  ~Socket() {
    if (fd >= 0) ::close(fd);
  }
};
namespace frame = rpc::frame;
// The helper relays Host RPC requests (terminal input) and replies (runtime
// snapshots) that hostd's own clients exchange in Host RPC frames, so its link
// accepts the same largest frame and nothing a client could not send.
constexpr size_t kMaxHelperFrameBytes = TransportLimits{}.max_frame_bytes;
auto HelperFailure(const std::string& why) -> EnvironmentFailure {
  return EnvironmentFailure("The VM helper connection closed: " + why, "");
}
auto Read(trace::Link& link, int fd) -> std::unique_ptr<V::MessageT> {
  std::optional<std::string> bytes;
  try {
    bytes = frame::Read(fd, kMaxHelperFrameBytes);
  } catch (const frame::Error& error) {
    throw HelperFailure(error.what());
  }
  if (!bytes) throw HelperFailure("end of stream");
  flatbuffers::Verifier verifier(
      reinterpret_cast<const uint8_t*>(bytes->data()), bytes->size());
  if (!V::VerifyMessageBuffer(verifier))
    throw std::runtime_error("Invalid VM helper message");
  trace::Observe(link, "VM helper", "hostd", false, *bytes,
                 &trace::kFlatBufferCodec);
  return std::unique_ptr<V::MessageT>(V::GetMessage(bytes->data())->UnPack());
}
template <class T>
auto Send(trace::Link& link, int fd, T value) -> void {
  V::RequestT request;
  request.value.Set(std::move(value));
  V::MessageT message;
  message.request_id = 1;
  message.payload.Set(std::move(request));
  flatbuffers::FlatBufferBuilder builder;
  V::FinishMessageBuffer(builder, V::Message::Pack(builder, &message));
  const std::string_view bytes(
      reinterpret_cast<const char*>(builder.GetBufferPointer()),
      builder.GetSize());
  try {
    frame::Write(fd, bytes, kMaxHelperFrameBytes);
  } catch (const frame::Error& error) {
    throw HelperFailure(error.what());
  }
  trace::Observe(link, "hostd", "VM helper", true, bytes,
                 &trace::kFlatBufferCodec);
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
        trace::Link link;
        Send(link, socket.fd, stop);
        Read(link, socket.fd);
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
    run.rpc_trace = trace::LaunchEnabled();
    run.mounts = Mounts(launch);
    auto reply = Call(std::move(run));
    const auto* process = reply->value.AsProcess();
    if (!process) throw std::runtime_error("Expected a VM project process");
    Remember(process->pid, launch.key);
    return process->pid;
  }
  auto Inspect(H::EnvironmentStateT& ready) -> std::optional<int64_t> override {
    V::ProcessRequestT request;
    request.key = ready.key;
    const auto process = CallProcess(std::move(request));
    if (process.endpoint.empty()) return std::nullopt;
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
    const auto key = Key(pid);
    std::lock_guard lock(status_mutex_);
    const auto found = statuses_.find(key);
    // A disconnected watcher alone cannot prove that a process exited.
    if (found == statuses_.end()) return std::nullopt;
    const auto& status = found->second;
    if (status.pid != pid) return -1;
    if (status.phase == G::ProjectPhase::Exited) return status.exit_status;
    return std::nullopt;
  }
  auto WaitForReady(const std::filesystem::path&, const std::string& key,
                    int64_t pid, std::stop_token stop,
                    const std::function<bool()>& done) -> void override {
    if (!pid) {
      // Rediscover a launch whose reply was lost. An inspect also waits for
      // the helper's subscription to exist before we use its status cache.
      V::ProcessRequestT request;
      request.key = key;
      const auto process = CallProcess(std::move(request));
      if (!process.running || !process.endpoint.empty()) {
        if (!stop.stop_requested()) done();
        return;
      }
      pid = process.pid;
    }
    {
      std::unique_lock lock(status_mutex_);
      if (!status_changed_.wait(lock, stop,
                                [&] {
                                  if (!watch_connected_) return true;
                                  const auto found = statuses_.find(key);
                                  return found != statuses_.end() &&
                                         found->second.pid == pid &&
                                         found->second.phase !=
                                             G::ProjectPhase::Starting;
                                }) ||
          stop.stop_requested())
        return;
      if (!watch_connected_)
        throw std::runtime_error(
            "VM helper stopped reporting project readiness");
    }
    if (!done())
      throw std::runtime_error(
          "The guest project endpoint is unavailable after startup");
  }
  auto Observe(std::stop_token stop, const std::function<void()>& check)
      -> void override {
    std::unique_lock lock(status_mutex_);
    auto seen = status_version_;
    while (!stop.stop_requested()) {
      lock.unlock();
      check();
      lock.lock();
      if (!status_changed_.wait(lock, stop,
                                [&] { return status_version_ != seen; }))
        return;
      seen = status_version_;
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
          "A resident project process cannot be authenticated", "");
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
    if (!runtime) throw std::runtime_error("Expected a VM runtime reply");
    return *runtime;
  }
  auto Terminal(const H::HostTerminalRequestT& request)
      -> H::HostTerminalT override {
    auto reply = Call(request);
    const auto* terminal = reply->value.AsHostTerminal();
    if (!terminal) throw std::runtime_error("Expected a VM terminal reply");
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
  std::jthread watcher_;
  std::map<int64_t, std::string> keys_;
  std::mutex status_mutex_;
  std::condition_variable_any status_changed_;
  std::map<std::string, G::ProjectStatusT> statuses_;
  uint64_t status_version_ = 0;
  bool watch_connected_ = false;

  auto OnStatus(const G::ProjectStatusT& status) -> void {
    {
      std::lock_guard lock(status_mutex_);
      statuses_[status.key] = status;
      ++status_version_;
    }
    status_changed_.notify_all();
  }
  auto SetWatchConnected(bool connected) -> void {
    {
      std::lock_guard lock(status_mutex_);
      watch_connected_ = connected;
    }
    status_changed_.notify_all();
  }
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
    const auto address = unix_socket::Address(path_);
    Socket socket{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (socket.fd < 0) return -1;
    ::fcntl(socket.fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    ::setsockopt(socket.fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
    if (::connect(socket.fd, reinterpret_cast<const sockaddr*>(&address),
                  sizeof address))
      return -1;
    const auto fd = socket.fd;
    socket.fd = -1;
    return fd;
  }
  auto Connect() -> int {
    std::lock_guard lock(open_mutex_);
    if (stopping_) throw std::runtime_error("VM adapter is stopping");
    if (auto fd = TryConnect(); fd >= 0) return fd;
    EnvironmentLaunch launch;
    launch.vm_helper = true;
    launch.arguments = {options_.binary.string(), "--support",
                        options_.support.string(), "--image",
                        options_.image.string()};
    launch.log = options_.support / "vmhost.log";
    auto exited = std::make_shared<std::atomic<bool>>(false);
    const auto pid = static_cast<pid_t>(LaunchNativeProcess(launch));
    // This helper can outlive its connection. Reap only the child we spawned,
    // without retaining the adapter or relying on a subsequent Watch or RPC.
    const auto reap = [pid, exited] {
      while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
      }
      *exited = true;
    };
    std::thread reaper;
    try {
      reaper = std::thread(reap);
    } catch (...) {
      ::kill(pid, SIGKILL);
      reap();
      throw;
    }
    reaper.detach();
    int connected = -1;
    WaitForNativeReadiness(path_, pid, startup_stop_.get_token(), [&] {
      connected = TryConnect();
      if (connected >= 0) return true;
      if (*exited)
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
    trace::Link link;
    try {
      Send(link, watch_fd_, V::WatchT{});
      // The helper acknowledges Watch before subscribing this peer.
      Read(link, watch_fd_);
    } catch (...) {
      ::close(watch_fd_);
      watch_fd_ = -1;
      throw;
    }
    SetWatchConnected(true);
    watcher_ =
        std::jthread([this, link, fd = watch_fd_.load()](std::stop_token stop) mutable {
          try {
            while (!stop.stop_requested()) {
              auto message = Read(link, fd);
              const auto* event = message->payload.AsEvent();
              if (!event)
                throw std::runtime_error("Expected a VM helper event");
              if (const auto* status = event->value.AsProjectStatus())
                OnStatus(*status);
              if (const auto* progress = event->value.AsProgress())
                options_.progress(*progress);
              if (const auto* runtime = event->value.AsHostRuntime())
                options_.runtime(*runtime);
              if (const auto* terminal = event->value.AsHostTerminalData())
                options_.terminal(*terminal);
            }
          } catch (const std::exception& error) {
            if (!stop.stop_requested())
              std::fprintf(stderr, "VM helper watch: %s\n", error.what());
          }
          SetWatchConnected(false);
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
    trace::Link link;
    Send(link, socket.fd, std::move(value));
    auto message = Read(link, socket.fd);
    auto* reply = message->payload.AsReply();
    if (!reply) throw std::runtime_error("Expected a VM helper reply");
    if (const auto* failure = reply->value.AsFailure())
      throw EnvironmentFailure(failure->message, failure->log);
    return std::make_unique<V::ReplyT>(*reply);
  }
  auto CallProcess(V::ProcessRequestT request) -> V::ProcessT {
    auto reply = Call(std::move(request));
    auto* process = reply->value.AsProcess();
    if (!process) throw std::runtime_error("Expected a VM project process");
    return std::move(*process);
  }
};
}  // namespace

auto VMEnvironmentProcess(VMProcessOptions options)
    -> std::shared_ptr<EnvironmentProcess> {
  return std::make_shared<VMProcess>(std::move(options));
}
}  // namespace tsugumi::host
