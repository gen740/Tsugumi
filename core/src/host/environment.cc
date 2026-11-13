#include "host/environment.hh"

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include "agent/json.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi::host {
namespace H = Tsugumi::Host;
namespace json = agent::json;
namespace {
using namespace std::chrono_literals;
auto Strings(const std::vector<std::string>& values) -> std::string {
  std::vector<std::string> encoded;
  for (const auto& value : values) encoded.push_back(json::Quote(value));
  return json::Encode(encoded);
}
auto PrivateDirectory(const std::filesystem::path& path) -> void {
  std::filesystem::create_directories(path);
  struct stat st{};
  if (::lstat(path.c_str(), &st) || !S_ISDIR(st.st_mode) ||
      st.st_uid != ::getuid())
    throw std::runtime_error(
        "Environment directory must belong to this user: " + path.string());
  if (::chmod(path.c_str(), 0700))
    throw std::runtime_error("Cannot make environment directory private: " +
                             path.string());
}
}  // namespace

struct Environments::Impl {
  struct Entry {
    std::mutex mutex;
    std::atomic<bool> starting = false;
    std::atomic<uint64_t> start_generation = 0;
    State state;
    std::vector<std::string> folders;
    std::vector<std::string> origins;
    int64_t pid = 0;
    bool child = false;
  };
  EnvironmentOptions options;
  EnvironmentAuthorization authorization;
  Changed changed;
  Progress progress;
  std::mutex mutex;
  std::map<std::string, std::shared_ptr<Entry>> entries;
  std::stop_source shutdown;
  std::jthread observer;

  Impl(EnvironmentOptions o, EnvironmentAuthorization a, Changed c, Progress p)
      : options(std::move(o)),
        authorization(std::move(a)),
        changed(std::move(c)),
        progress(std::move(p)) {
    if (!options.process) options.process = NativeEnvironmentProcess();
    PrivateDirectory(options.directory);
    PrivateDirectory(options.socket_directory);
    observer = std::jthread([this](std::stop_token stop) {
      while (!stop.stop_requested()) {
        std::vector<std::shared_ptr<Entry>> snapshot;
        {
          std::lock_guard lock(mutex);
          for (const auto& [_, entry] : entries) snapshot.push_back(entry);
        }
        for (const auto& entry : snapshot) {
          std::unique_lock lock(entry->mutex, std::try_to_lock);
          if (lock.owns_lock() &&
              entry->state.phase == H::EnvironmentPhase::Ready) {
            try {
              CheckDeath(*entry);
            } catch (const std::exception&) {
              // A later request retries revocation/notification after an I/O
              // failure.
            }
          }
        }
        std::this_thread::sleep_for(100ms);
      }
    });
  }
  ~Impl() { Shutdown(); }
  auto Shutdown() -> void {
    shutdown.request_stop();
    observer.request_stop();
    if (observer.joinable()) observer.join();
  }
  auto EntryFor(const std::string& project) -> std::shared_ptr<Entry> {
    const auto path = std::filesystem::canonical(project);
    struct stat st{};
    if (::stat(path.c_str(), &st) || !S_ISDIR(st.st_mode) ||
        !std::filesystem::is_regular_file(path / "tsugumi.json"))
      throw std::runtime_error("Not a Tsugumi project: " + path.string());
    const auto key = crypto::sha256_hex(std::to_string(st.st_dev) + ":" +
                                        std::to_string(st.st_ino))
                         .substr(0, 16);
    std::lock_guard lock(mutex);
    auto& entry = entries[key];
    if (!entry) {
      entry = std::make_shared<Entry>();
      entry->state.key = key;
      entry->state.project = path.string();
      entry->state.control_dir = (options.directory / key / "control").string();
      entry->state.ready_file =
          (std::filesystem::path(entry->state.control_dir) / "server.ready")
              .string();
      // A State query may rediscover a resident daemon after agentd restarts.
    }
    return entry;
  }
  auto Notify(Entry& entry) -> void { changed(entry.state); }
  auto Stage(const Entry& entry, const std::string& stage) -> void {
    H::EnvironmentProgressT event;
    event.project = entry.state.project;
    event.key = entry.state.key;
    event.stage = stage;
    progress(event);
  }
  auto Revoke(Entry& entry) -> void {
    authorization.revoke(entry.state.key);
    std::filesystem::remove(std::filesystem::path(entry.state.control_dir) /
                            "execution-broker-capability");
  }
  auto ClearEndpoint(Entry& entry) -> void {
    entry.state.endpoint.clear();
    entry.state.token.clear();
    entry.state.project_id.clear();
    entry.state.server_instance_id.clear();
    entry.state.unix_socket.clear();
    entry.state.pid = 0;
  }
  auto Failed(Entry& entry, const std::string& message) -> void {
    Revoke(entry);
    ClearEndpoint(entry);
    entry.state.phase = H::EnvironmentPhase::Failed;
    entry.state.message = message;
    entry.state.recovery = "Check the log and retry.";
    const auto log =
        std::filesystem::path(entry.state.control_dir) / "server.log";
    std::error_code error;
    const auto size = std::filesystem::file_size(log, error);
    entry.state.log =
        error ? ""
              : fs::read_range(log, size > 65536 ? size - 65536 : 0, 65536)
                    .value_or("");
    entry.state.startup_error_json =
        fs::read_file(std::filesystem::path(entry.state.control_dir) /
                      "server.startup-error.json")
            .value_or("");
    if (entry.state.log.find("already owns") != std::string::npos)
      entry.state.recovery =
          "Another tsugumid owns this project. Stop it and retry.";
    Notify(entry);
  }
  auto Alive(int64_t pid, bool child) -> bool {
    return !options.process->Poll(pid, child).has_value();
  }
  auto CheckDeath(Entry& entry) -> bool {
    if (!entry.pid) return false;
    const auto status = options.process->Poll(entry.pid, entry.child);
    if (!status) return false;
    if (entry.state.phase == H::EnvironmentPhase::Ready && *status >= 0 &&
        ((WIFEXITED(*status) && WEXITSTATUS(*status) == 0) ||
         (WIFSIGNALED(*status) && WTERMSIG(*status) == SIGTERM))) {
      Revoke(entry);
      ClearEndpoint(entry);
      entry.state.phase = H::EnvironmentPhase::Stopped;
      entry.state.message.clear();
      Notify(entry);
    } else {
      Failed(entry, "tsugumid exited. Retry to start it again.");
    }
    entry.pid = 0;
    entry.child = false;
    return true;
  }
  auto ReadReady(Entry& entry) -> bool {
    try {
      const auto raw = fs::read_file(entry.state.ready_file);
      if (!raw) return false;
      const auto object = json::Object(*raw);
      State ready(entry.state);
      ready.pid = static_cast<int64_t>(json::UInt(json::Get(object, "pid")));
      ready.endpoint = json::Text(object, "endpoint");
      ready.token = json::Text(object, "token");
      ready.project_id = json::Text(object, "project_id");
      ready.server_instance_id = json::Text(object, "server_instance_id");
      ready.unix_socket = json::Text(object, "unix_socket");
      if (json::UInt(json::Get(object, "protocol")) != 1 || ready.pid <= 0 ||
          ready.token.empty() || ready.project_id.empty() ||
          ready.server_instance_id.empty() || ready.unix_socket.empty() ||
          (entry.pid && entry.pid != ready.pid) ||
          !Alive(ready.pid, entry.child) || !options.process->Verify(ready))
        return false;
      entry.pid = ready.pid;
      ready.phase = H::EnvironmentPhase::Ready;
      ready.message.clear();
      ready.recovery.clear();
      ready.log.clear();
      ready.startup_error_json.clear();
      entry.state = std::move(ready);
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }
  auto Authorize(Entry& entry) -> H::ExecutionAuthorizationT {
    auto grant = authorization.authorize(entry.state.key, entry.state.project,
                                         entry.state.control_dir);
    entry.state.execution_scope_unavailable = !grant.available;
    return grant;
  }
  auto WriteFolders(Entry& entry, const std::vector<std::string>& folders)
      -> void {
    fs::write_atomic(
        std::filesystem::path(entry.state.control_dir) / "attached.json",
        json::Encode(json::Members{{"folders", Strings(folders)}}), 0600);
    entry.folders = folders;
  }
  auto Start(Entry& entry, const H::EnvironmentRequestT& request) -> void {
    entry.starting = true;
    ++entry.start_generation;
    struct Finished {
      Entry& entry;
      ~Finished() { entry.starting = false; }
    } finished{entry};
    PrivateDirectory(
        std::filesystem::path(entry.state.control_dir).parent_path());
    PrivateDirectory(entry.state.control_dir);
    entry.origins = request.origins;
    fs::write_atomic(
        std::filesystem::path(entry.state.control_dir) / "origins.json",
        json::Encode(json::Members{{"origins", Strings(request.origins)}}),
        0600);
    WriteFolders(entry, request.folders);
    CheckDeath(entry);
    if (ReadReady(entry)) {
      Authorize(entry);
      Notify(entry);
      return;
    }
    // A previously known process with an unresponsive endpoint still owns the
    // project. Do not spawn a second one or destroy its control files.
    if (entry.pid && Alive(entry.pid, entry.child))
      throw std::runtime_error(
          "tsugumid is alive but its endpoint is "
          "unavailable; stop it before retrying");
    entry.state.phase = H::EnvironmentPhase::Starting;
    entry.state.message = "Starting tsugumid";
    entry.state.log.clear();
    entry.state.startup_error_json.clear();
    Notify(entry);
    Stage(entry, "starting tsugumid");
    try {
      const auto control = std::filesystem::path(entry.state.control_dir);
      const auto sockets = options.socket_directory / entry.state.key;
      PrivateDirectory(sockets);
      for (const auto* file : {"server.ready", "server.startup-error.json"})
        std::filesystem::remove(control / file);
      std::filesystem::remove(sockets / "s");
      if (!std::filesystem::exists(control / "grants.json"))
        fs::write_atomic(control / "grants.json", R"({"grants":{}})", 0600);
      const auto roots = options.directory.parent_path() / "build-roots";
      const auto scratch = std::filesystem::path("/var/tmp") /
                           ("tsugumi-scratch-" + std::to_string(::getuid()));
      EnvironmentLaunch launch;
      launch.arguments = {
          options.tsugumid.string(),
          "--project",
          entry.state.project,
          "--listen",
          "127.0.0.1:0",
          "--unix",
          (sockets / "s").string(),
          "--control-dir",
          control.string(),
          "--worker",
          options.worker.string(),
          "--sdk-dir",
          options.sdk.string(),
          "--cpus",
          std::to_string(std::max(1u, std::thread::hardware_concurrency())),
          "--run-scratch-dir",
          (scratch / entry.state.key).string(),
          "--gcroots-dir",
          (roots / entry.state.key).string(),
          "--usage-roots-dir",
          roots.string(),
          "--usage-scratch-dir",
          scratch.string()};
      if (request.archive_incompatible_history)
        launch.arguments.push_back("--archive-incompatible-history");
      for (const auto& origin : request.origins) {
        launch.arguments.push_back("--allow-origin");
        launch.arguments.push_back(origin);
      }
      launch.log = control / "server.log";
      launch.authorization = Authorize(entry);
      entry.pid = options.process->Launch(launch);
      entry.child = true;
      Stage(entry, "waiting for tsugumid to become ready");
      const auto deadline =
          std::chrono::steady_clock::now() + options.start_timeout;
      while (!shutdown.stop_requested() &&
             std::chrono::steady_clock::now() < deadline) {
        if (CheckDeath(entry)) return;
        if (ReadReady(entry)) {
          Notify(entry);
          return;
        }
        std::this_thread::sleep_for(50ms);
      }
      Stop(entry);
      Failed(entry, shutdown.stop_requested()
                        ? "Environment start cancelled"
                        : "tsugumid did not become ready within 60 seconds");
    } catch (const std::exception& error) {
      if (entry.pid) Stop(entry);
      Failed(entry, error.what());
    }
  }
  auto Stop(Entry& entry) -> void {
    // State/Stop can be the first operation after an agentd restart.
    if (!entry.pid) ReadReady(entry);
    Revoke(entry);
    if (entry.pid && !entry.child && !options.process->Verify(entry.state))
      entry.pid = 0;
    if (entry.pid) {
      options.process->Signal(entry.pid, SIGTERM);
      auto deadline = std::chrono::steady_clock::now() + options.stop_timeout;
      while (Alive(entry.pid, entry.child) &&
             std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(50ms);
      if (Alive(entry.pid, entry.child)) {
        options.process->Signal(entry.pid, SIGKILL);
        deadline = std::chrono::steady_clock::now() + 5s;
        while (Alive(entry.pid, entry.child) &&
               std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(10ms);
        if (Alive(entry.pid, entry.child))
          throw std::runtime_error("tsugumid did not stop");
      }
    }
    entry.pid = 0;
    entry.child = false;
    std::filesystem::remove(entry.state.ready_file);
    ClearEndpoint(entry);
    entry.state.phase = H::EnvironmentPhase::Stopped;
    entry.state.message.clear();
    entry.state.recovery.clear();
    entry.state.log.clear();
    entry.state.startup_error_json.clear();
    Notify(entry);
  }
};

Environments::Environments(EnvironmentOptions options,
                           EnvironmentAuthorization authorization,
                           Changed changed, Progress progress)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(authorization),
                                   std::move(changed), std::move(progress))) {}
Environments::~Environments() = default;
auto Environments::Shutdown() -> void { impl_->Shutdown(); }
auto Environments::Runtime() const -> H::HostRuntimeT {
  H::HostRuntimeT runtime;
  runtime.vm_state = "unavailable";
  runtime.store_path = "/nix/store";
  runtime.store_exists = std::filesystem::is_directory(runtime.store_path);
  return runtime;
}
auto Environments::Handle(const H::EnvironmentRequestT& request) -> State {
  if (impl_->shutdown.stop_requested())
    throw std::runtime_error("Host service is stopping");
  auto entry = impl_->EntryFor(request.project);
  const auto generation = entry->start_generation.load();
  const bool joining_start = entry->starting.load();
  std::lock_guard lock(entry->mutex);
  // Concurrent callers share a failed attempt too. A later explicit retry
  // starts again; a ready result may still refresh origins and folder grants.
  if (request.action == H::EnvironmentAction::Start &&
      (joining_start || entry->start_generation.load() != generation) &&
      entry->state.phase != H::EnvironmentPhase::Ready)
    return entry->state;
  const bool renamed = entry->state.project !=
                       std::filesystem::canonical(request.project).string();
  entry->state.project = std::filesystem::canonical(request.project).string();
  switch (request.action) {
    case H::EnvironmentAction::Start:
      impl_->Start(*entry, request);
      break;
    case H::EnvironmentAction::Stop:
      impl_->Stop(*entry);
      break;
    case H::EnvironmentAction::Folders:
      PrivateDirectory(
          std::filesystem::path(entry->state.control_dir).parent_path());
      PrivateDirectory(entry->state.control_dir);
      impl_->WriteFolders(*entry, request.folders);
      [[fallthrough]];
    case H::EnvironmentAction::State:
      if (!impl_->CheckDeath(*entry)) {
        if (!entry->pid) {
          if (impl_->ReadReady(*entry)) {
            impl_->Authorize(*entry);
            impl_->Notify(*entry);
          } else if (entry->state.phase == H::EnvironmentPhase::Stopped) {
            impl_->Revoke(*entry);
            impl_->Notify(*entry);
          }
        } else if (renamed &&
                   entry->state.phase == H::EnvironmentPhase::Ready) {
          impl_->Notify(*entry);
        }
      }
      break;
    default:
      throw std::runtime_error("Unknown environment action");
  }
  return entry->state;
}
}  // namespace tsugumi::host
