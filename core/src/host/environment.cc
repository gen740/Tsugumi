#include "host/environment.hh"

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include "agent/json.hh"
#include "host/vm_process.hh"
#include "host_requirements.hh"
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
    std::optional<Tsugumi::RPC::ProjectHostRequirementsT> requirements;
    std::vector<EnvironmentMount> mounted;
    std::vector<std::string> folders;
    std::vector<std::string> origins;
    int64_t pid = 0;
    bool child = false;
    bool idle_stopping = false;
    std::vector<std::string> secrets;
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
        std::this_thread::sleep_for(options.process->Guest() ? 1000ms : 100ms);
      }
    });
  }
  ~Impl() { Shutdown(); }
  auto Shutdown() -> void {
    shutdown.request_stop();
    observer.request_stop();
    // Close adapter calls before joining the observer: a guest status request
    // may be stuck while shutdown/maintenance owns the guest control path.
    options.process->Shutdown();
    if (observer.joinable()) observer.join();
    if (!options.process->Guest()) {
      std::vector<std::shared_ptr<Entry>> snapshot;
      {
        std::lock_guard lock(mutex);
        for (const auto& [_, entry] : entries) snapshot.push_back(entry);
      }
      for (const auto& entry : snapshot) {
        std::lock_guard entry_lock(entry->mutex);
        try { Stop(*entry); } catch (const std::exception&) {}
      }
    }
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
      for (const auto* name : {"origins", "folders"}) {
        try {
          const auto file = std::filesystem::path(entry->state.control_dir) /
                            (std::string(name) == "origins" ? "origins.json"
                                                            : "attached.json");
          if (const auto raw = fs::read_file(file))
            for (const auto& value :
                 json::Array(json::Get(json::Object(*raw), name, "[]")))
              (std::string(name) == "origins" ? entry->origins : entry->folders)
                  .push_back(json::String(value));
        } catch (const std::exception&) {
        }
      }
      entry->state.folders = entry->folders;
      // A State query may rediscover a resident daemon after hostd restarts.
    }
    return entry;
  }
  auto Notify(Entry& entry) -> void {
    entry.state.attached_roots.clear();
    if (entry.state.phase == H::EnvironmentPhase::Ready) {
      for (const auto& mount : entry.mounted) {
        if (!mount.name.starts_with("chat-")) continue;
        auto root = std::make_unique<H::AttachedRootT>();
        root->host_path = mount.path;
        root->execution_path = "/run/tsugumi/chat/" + mount.name.substr(5);
        entry.state.attached_roots.push_back(std::move(root));
      }
      if (!options.process->Guest()) for (const auto& folder : entry.folders) {
        auto root = std::make_unique<H::AttachedRootT>();
        root->host_path = folder;
        root->execution_path = folder;
        entry.state.attached_roots.push_back(std::move(root));
      }
    }
    changed(entry.state);
  }
  auto Stage(const Entry& entry, const std::string& stage) -> void {
    H::EnvironmentProgressT event;
    event.project = entry.state.project;
    event.key = entry.state.key;
    event.stage = stage;
    progress(event);
  }
  auto RememberSecret(Entry& entry, const std::string& secret) -> void {
    if (!secret.empty() && std::find(entry.secrets.begin(), entry.secrets.end(),
                                     secret) == entry.secrets.end())
      entry.secrets.push_back(secret);
    std::sort(entry.secrets.begin(), entry.secrets.end(),
              [](const auto& a, const auto& b) { return a.size() > b.size(); });
  }
  auto RememberSecrets(Entry& entry) -> void {
    RememberSecret(entry, entry.state.token);
    try {
      if (const auto raw = fs::read_file(entry.state.ready_file))
        RememberSecret(entry, json::Text(json::Object(*raw), "token"));
    } catch (const std::exception&) {
    }
    RememberSecret(
        entry, fs::read_file(std::filesystem::path(entry.state.control_dir) /
                             "execution-broker-capability")
                   .value_or(""));
  }
  auto Redact(const Entry& entry, std::string text) -> std::string {
    for (const auto& secret : entry.secrets) {
      size_t offset = 0;
      while ((offset = text.find(secret, offset)) != std::string::npos) {
        // Preserve byte positions so bounding the tail cannot expose the end
        // of a credential that crossed the read boundary.
        text.replace(offset, secret.size(), secret.size(), '*');
        offset += secret.size();
      }
    }
    return text;
  }
  auto LogTail(const Entry& entry) -> std::string {
    const auto log = std::filesystem::path(entry.state.control_dir) /
                     (options.process->Guest() ? "tsugumid.log" : "server.log");
    std::error_code error;
    const auto size = std::filesystem::file_size(log, error);
    if (error) return "";
    // Include enough prefix to redact a credential straddling the tail
    // boundary.
    size_t limit = 65536;
    for (const auto& secret : entry.secrets)
      limit = std::max(limit, 65536 + secret.size());
    auto text = Redact(
        entry, fs::read_range(log, size > limit ? size - limit : 0, limit)
                   .value_or(""));
    if (text.size() > 65536) text.erase(0, text.size() - 65536);
    return text;
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
  auto Failed(Entry& entry, const std::string& message,
              const EnvironmentFailure* failure = nullptr,
              H::EnvironmentFailureCode code = H::EnvironmentFailureCode::LaunchFailed) -> void {
    RememberSecrets(entry);
    Revoke(entry);
    ClearEndpoint(entry);
    entry.state.phase = H::EnvironmentPhase::Failed;
    entry.state.failure_code = code;
    entry.state.recovery =
        failure ? failure->recovery : "Check the log and retry.";
    entry.state.log = failure ? Redact(entry, failure->log) : LogTail(entry);
    if (entry.state.log.size() > 65536)
      entry.state.log.erase(0, entry.state.log.size() - 65536);
    entry.state.message = Redact(entry, message);
    if (!entry.state.log.empty()) {
      const auto& log = entry.state.log;
      entry.state.message +=
          "\n" + log.substr(log.size() > 4096 ? log.size() - 4096 : 0);
    }
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
    if (entry.state.phase == H::EnvironmentPhase::Ready &&
        (entry.idle_stopping || (*status >= 0 && ((WIFEXITED(*status) && WEXITSTATUS(*status) == 0) ||
         (WIFSIGNALED(*status) && WTERMSIG(*status) == SIGTERM))))) {
      Revoke(entry);
      ClearEndpoint(entry);
      entry.state.phase = H::EnvironmentPhase::Stopped;
      entry.state.message.clear();
      Notify(entry);
    } else {
      Failed(entry, "tsugumid exited. Retry to start it again.", nullptr,
             H::EnvironmentFailureCode::ProcessExit);
    }
    entry.pid = 0;
    entry.child = false;
    entry.idle_stopping = false;
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
      RememberSecret(entry, ready.token);
      ready.project_id = json::Text(object, "project_id");
      ready.server_instance_id = json::Text(object, "server_instance_id");
      ready.unix_socket = json::Text(object, "unix_socket");
      if (json::UInt(json::Get(object, "protocol")) != 1 || ready.pid <= 0 ||
          ready.token.empty() || ready.project_id.empty() ||
          ready.server_instance_id.empty() ||
          (!options.process->Guest() && ready.unix_socket.empty()) ||
          (!options.process->Guest() && entry.pid && entry.pid != ready.pid))
        return false;
      const auto handle = options.process->Inspect(ready);
      if (!handle || (entry.pid && entry.pid != *handle) ||
          !Alive(*handle, entry.child))
        return false;
      if (options.process->Guest() && entry.mounted.empty()) {
        // Restore actual shares after a hostd restart, never guess from paths
        // currently present in either filesystem.
        const auto saved = fs::read_file(
            std::filesystem::path(entry.state.control_dir) / "mounted-folders.json");
        if (!saved) return false;
        std::vector<EnvironmentMount> mounted;
        for (const auto& raw : json::Array(*saved)) {
          const auto mount = json::Object(raw);
          mounted.push_back({json::Text(mount, "name"), json::Text(mount, "path"),
                             json::Bool(json::Get(mount, "read_only"))});
        }
        entry.mounted = std::move(mounted);
      }
      entry.pid = *handle;
      ready.phase = H::EnvironmentPhase::Ready;
      ready.message.clear();
      ready.recovery.clear();
      ready.log.clear();
      ready.startup_error_json.clear();
      ready.failure_code = H::EnvironmentFailureCode::None;
      entry.state = std::move(ready);
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }
  auto Authorize(Entry& entry) -> H::ExecutionAuthorizationT {
    auto grant = authorization.authorize(entry.state.key, entry.state.project,
                                         entry.state.control_dir);
    RememberSecret(entry, fs::read_file(grant.capability_file).value_or(""));
    entry.state.execution_scope_unavailable = !grant.available;
    return grant;
  }
  auto WriteFolders(Entry& entry, const std::vector<std::string>& folders)
      -> void {
    fs::write_atomic(
        std::filesystem::path(entry.state.control_dir) / "attached.json",
        json::Encode(json::Members{{"folders", Strings(folders)}}), 0600);
    entry.folders = folders;
    entry.state.folders = folders;
  }
  auto Plan(const Entry& entry) -> EnvironmentLaunch {
    EnvironmentLaunch launch;
    launch.key = entry.state.key;
    launch.project = entry.state.project;
    launch.control = entry.state.control_dir;
    launch.origins = entry.origins;
    launch.folders = entry.folders;
    launch.mounts = {{"project", launch.project, false},
                     {"control", launch.control, false}};
    if (options.process->Guest()) {
      std::map<std::string, bool> host_folders;
      if (entry.requirements)
        for (const auto& folder : entry.requirements->folders)
          if (std::filesystem::is_directory(folder->path))
            host_folders[folder->path] = host_folders[folder->path] || folder->write;
      for (const auto& [path, write] : host_folders)
        launch.mounts.push_back(
            {"host-" + crypto::sha256_hex(path).substr(0, 16), path, !write});
      for (const auto& path : entry.folders)
        if (path.starts_with("/") && std::filesystem::is_directory(path))
          launch.mounts.push_back(
              {"chat-" + crypto::sha256_hex(path).substr(0, 16), path, true});
    }
    return launch;
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
    if (options.process->Guest())
      ::chmod(entry.state.control_dir.c_str(), 0755);
    if (options.process->Guest()) {
      for (const auto& origin : request.origins)
        if (std::find(entry.origins.begin(), entry.origins.end(), origin) ==
            entry.origins.end())
          entry.origins.push_back(origin);
      std::sort(entry.origins.begin(), entry.origins.end());
    } else
      entry.origins = request.origins;
    fs::write_atomic(
        std::filesystem::path(entry.state.control_dir) / "origins.json",
        json::Encode(json::Members{{"origins", Strings(entry.origins)}}), 0600);
    WriteFolders(entry, entry.folders);
    CheckDeath(entry);
    if (ReadReady(entry)) {
      const auto previous = entry.mounted.empty() ? Plan(entry).mounts : entry.mounted;
      entry.requirements = options.process->Requirements(entry.state);
      const auto next = Plan(entry);
      // The guest keeps the folders of its start: replacing the shares of a
      // running project leaves it with files it can no longer reach, so a
      // different plan starts the project again.
      if (options.process->Guest() && next.mounts != previous) {
        Stop(entry);
      } else {
        entry.mounted = next.mounts;
        Authorize(entry);
        Notify(entry);
        return;
      }
    }
    // A previously known process with an unresponsive endpoint still owns the
    // project. Do not spawn a second one or destroy its control files.
    if (entry.pid && Alive(entry.pid, entry.child))
      throw std::runtime_error(
          "tsugumid is alive but its endpoint is "
          "unavailable; stop it before retrying");
    if (options.prepare_project_identity)
      options.prepare_project_identity(entry.state.project);
    options.process->GuardStart(entry.state.key);
    entry.state.phase = H::EnvironmentPhase::Starting;
    entry.idle_stopping = false;
    entry.state.message = "Starting tsugumid";
    entry.state.log.clear();
    entry.state.startup_error_json.clear();
    entry.state.failure_code = H::EnvironmentFailureCode::None;
    Notify(entry);
    Stage(entry, "starting tsugumid");
    try {
      const auto control = std::filesystem::path(entry.state.control_dir);
      const auto sockets = options.socket_directory / entry.state.key;
      PrivateDirectory(sockets);
      for (const auto* file : {"server.ready", "server.startup-error.json"})
        std::filesystem::remove(control / file);
      std::filesystem::remove(sockets / "s");
      const auto roots = options.directory.parent_path() / "build-roots";
      const auto scratch = options.run_scratch_directory.empty()
                               ? fs::run_scratch_root()
                               : options.run_scratch_directory;
      if (options.process->Guest() && !entry.requirements)
        entry.requirements = load_host_requirements(entry.state.project);
      auto launch = Plan(entry);
      launch.archive = request.archive_incompatible_history;
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
      // Temporaries stay in the system's temporary folder unless the caller
      // selected its own run storage; then they go beside the control files.
      if (!options.run_scratch_directory.empty() ||
          std::getenv("TSUGUMI_RUN_SCRATCH_DIR")) {
        launch.arguments.push_back("--scratch-dir");
        launch.arguments.push_back((control / "scratch").string());
      }
      if (request.archive_incompatible_history)
        launch.arguments.push_back("--archive-incompatible-history");
      for (const auto& origin : request.origins) {
        launch.arguments.push_back("--allow-origin");
        launch.arguments.push_back(origin);
      }
      launch.log = control / "server.log";
      launch.authorization = Authorize(entry);
      std::vector<std::string> mounted;
      for (const auto& mount : launch.mounts)
        mounted.push_back(json::Encode(json::Members{
            {"name", json::Quote(mount.name)}, {"path", json::Quote(mount.path)},
            {"read_only", mount.read_only ? "true" : "false"}}));
      fs::write_atomic(control / "mounted-folders.json", json::Encode(mounted), 0600);
      // Recorded before the launch: a launch that fails after the process has
      // started (its reply was lost) leaves a process holding these mounts.
      entry.mounted = launch.mounts;
      entry.pid = options.process->Launch(launch);
      entry.child = true;
      Stage(entry, "waiting for tsugumid to become ready");
      const auto deadline =
          std::chrono::steady_clock::now() + options.start_timeout;
      while (!shutdown.stop_requested()) {
        if (CheckDeath(entry)) return;
        if (ReadReady(entry)) {
          auto accepted = options.process->Requirements(entry.state);
          const auto& planned = launch.mounts;
          entry.requirements = std::move(accepted);
          const auto required = Plan(entry).mounts;
          if (options.process->Guest() && planned != required)
            throw std::runtime_error("Host requirements changed during startup; retry the environment");
          Notify(entry);
          return;
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(50ms);
      }
      Stop(entry);
      Failed(entry, shutdown.stop_requested()
                        ? "Environment start cancelled"
                        : "tsugumid did not become ready within 60 seconds", nullptr,
             shutdown.stop_requested() ? H::EnvironmentFailureCode::Cancelled
                                       : H::EnvironmentFailureCode::ReadinessTimeout);
    } catch (const std::exception& error) {
      if (entry.pid) Stop(entry);
      Failed(entry, error.what(),
             dynamic_cast<const EnvironmentFailure*>(&error));
    }
  }
  // A guest launch whose reply was lost may have left a process this entry
  // holds no pid for. Adopt it once it is ready, or learn that nothing runs,
  // so that the caller can judge its mounts and stop it.
  auto Rediscover(Entry& entry) -> void {
    if (entry.pid || !options.process->Guest()) return;
    const auto deadline =
        std::chrono::steady_clock::now() + options.start_timeout;
    while (!shutdown.stop_requested() && !ReadReady(entry)) {
      try {
        options.process->GuardStart(entry.state.key);
        return;
      } catch (const EnvironmentFailure&) {
        if (std::chrono::steady_clock::now() >= deadline) throw;
      }
      std::this_thread::sleep_for(50ms);
    }
  }
  auto Stop(Entry& entry) -> void {
    RememberSecrets(entry);
    // State/Stop can be the first operation after an hostd restart.
    if (!entry.pid) ReadReady(entry);
    Revoke(entry);
    if (entry.pid && !entry.child && !options.process->Guest() &&
        !options.process->Verify(entry.state))
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
    entry.idle_stopping = false;
    std::filesystem::remove(entry.state.ready_file);
    ClearEndpoint(entry);
    entry.state.phase = H::EnvironmentPhase::Stopped;
    entry.state.message.clear();
    entry.state.recovery.clear();
    entry.state.log.clear();
    entry.state.startup_error_json.clear();
    entry.state.failure_code = H::EnvironmentFailureCode::None;
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
auto Environments::DrainIdle() -> void {
  std::vector<std::shared_ptr<Impl::Entry>> snapshot;
  {
    std::lock_guard lock(impl_->mutex);
    for (const auto& [_, entry] : impl_->entries) snapshot.push_back(entry);
  }
  for (const auto& entry : snapshot) {
    std::unique_lock lock(entry->mutex, std::try_to_lock);
    if (!lock || entry->starting || !entry->pid || entry->idle_stopping) continue;
    try { entry->idle_stopping = impl_->options.process->StopIfIdle(entry->state); } catch (const std::exception&) {}
  }
}
auto Environments::Idle() -> bool {
  std::lock_guard lock(impl_->mutex);
  for (const auto& [_, entry] : impl_->entries) {
    std::unique_lock entry_lock(entry->mutex, std::try_to_lock);
    if (!entry_lock || entry->starting || entry->pid) return false;
  }
  return true;
}
auto EnvironmentProcess::Runtime(const H::HostRuntimeRequestT& request)
    -> H::HostRuntimeT {
  if (request.action != H::RuntimeAction::State)
    throw std::runtime_error("This operation requires the VM runtime");
  H::HostRuntimeT runtime;
  runtime.vm_state = "unavailable";
  runtime.store_path = "/nix/store";
  runtime.store_exists = std::filesystem::is_directory(runtime.store_path);
  return runtime;
}
auto EnvironmentProcess::Terminal(const H::HostTerminalRequestT&)
    -> H::HostTerminalT {
  throw std::runtime_error("This terminal requires the VM runtime");
}
auto Environments::Runtime(const H::HostRuntimeRequestT& request)
    -> H::HostRuntimeT {
  auto input = request;
  if (input.action == H::RuntimeAction::DeleteEnvironment) {
    H::EnvironmentRequestT stop;
    stop.project = input.project;
    stop.action = H::EnvironmentAction::Stop;
    const auto state = Handle(stop);
    input.project_id = state.key;
    input.action = H::RuntimeAction::DeleteHistory;
    auto result = impl_->options.process->Runtime(input);
    std::filesystem::remove_all(
        std::filesystem::path(state.control_dir).parent_path());
    return result;
  }
  if (input.action == H::RuntimeAction::Shutdown ||
      input.action == H::RuntimeAction::Reset ||
      input.action == H::RuntimeAction::VerifyStore) {
    std::vector<std::shared_ptr<Impl::Entry>> entries;
    {
      std::lock_guard lock(impl_->mutex);
      for (const auto& [_, entry] : impl_->entries) entries.push_back(entry);
    }
    for (const auto& entry : entries) {
      std::lock_guard lock(entry->mutex);
      if (impl_->options.process->Guest() && input.action != H::RuntimeAction::VerifyStore)
        impl_->Revoke(*entry);
      else
        impl_->Stop(*entry);
    }
  }
  auto result = impl_->options.process->Runtime(input);
  if (impl_->options.process->Guest() &&
      (input.action == H::RuntimeAction::Shutdown || input.action == H::RuntimeAction::Reset)) {
    // vmd stops all projects concurrently under the VM's one deadline. Do not
    // spend a separate stop deadline per project before asking it to power off.
    std::vector<std::shared_ptr<Impl::Entry>> snapshot;
    {
      std::lock_guard lock(impl_->mutex);
      for (const auto& [_, entry] : impl_->entries) snapshot.push_back(entry);
    }
    for (const auto& entry : snapshot) {
      std::lock_guard entry_lock(entry->mutex);
      entry->pid = 0;
      entry->child = false;
      entry->idle_stopping = false;
      impl_->ClearEndpoint(*entry);
      entry->state.phase = H::EnvironmentPhase::Stopped;
      impl_->Notify(*entry);
    }
  }
  DescribeVMReset(result);
  if (input.action == H::RuntimeAction::Reset) {
    for (const auto& entry :
         std::filesystem::directory_iterator(impl_->options.directory)) {
      if (!entry.path().filename().string().starts_with(".")) {
        std::filesystem::remove_all(entry.path());
        ++result.environments;
      }
    }
  }
  return result;
}
auto Environments::Terminal(const H::HostTerminalRequestT& request)
    -> H::HostTerminalT {
  auto input = request;
  if (input.action == H::TerminalAction::Open) {
    H::EnvironmentRequestT state;
    state.project = input.project;
    {
      std::lock_guard lock(impl_->mutex);
      if (const auto found = impl_->entries.find(input.project);
          found != impl_->entries.end())
        state.project = found->second->state.project;
    }
    const auto ready = Handle(state);
    if (ready.phase != H::EnvironmentPhase::Ready)
      throw std::runtime_error("The project's environment is not running");
    input.project = impl_->options.process->Guest() ? ready.key : ready.project;
  }
  return impl_->options.process->Terminal(input);
}
auto Environments::Attach(
    const H::AttachFoldersRequestT& request,
    const std::function<void(const State&, const std::vector<std::string>&)>&
        commit) -> State {
  if (impl_->shutdown.stop_requested())
    throw std::runtime_error("Host service is stopping");
  std::vector<std::string> selected;
  for (const auto& value : request.folders) {
    const auto path = std::filesystem::path(value).lexically_normal();
    if (!path.is_absolute() || !std::filesystem::is_directory(path))
      throw std::runtime_error("Folder not found: " + value);
    const auto folder = path.string();
    if (std::find(selected.begin(), selected.end(), folder) == selected.end())
      selected.push_back(folder);
  }
  if (selected.empty()) throw std::runtime_error("Select at least one folder");
  auto entry = impl_->EntryFor(request.project);
  std::lock_guard lock(entry->mutex);
  entry->state.project = std::filesystem::canonical(request.project).string();
  impl_->CheckDeath(*entry);
  if (!entry->pid) impl_->ReadReady(*entry);
  const auto original = entry->folders;
  const bool was_ready = entry->state.phase == H::EnvironmentPhase::Ready;
  if (was_ready)
    entry->requirements = impl_->options.process->Requirements(entry->state);
  const auto previous_mounts =
      entry->mounted.empty() ? impl_->Plan(*entry).mounts : entry->mounted;
  auto folders = original;
  for (const auto& folder : selected)
    if (std::find(folders.begin(), folders.end(), folder) == folders.end())
      folders.push_back(folder);
  PrivateDirectory(
      std::filesystem::path(entry->state.control_dir).parent_path());
  PrivateDirectory(entry->state.control_dir);
  H::EnvironmentRequestT start;
  start.project = entry->state.project;
  start.origins = entry->origins;
  try {
    impl_->WriteFolders(*entry, folders);
    if (was_ready && impl_->options.process->Guest() &&
        impl_->Plan(*entry).mounts != previous_mounts)
      impl_->Stop(*entry);
    impl_->Start(*entry, start);
    if (entry->state.phase != H::EnvironmentPhase::Ready)
      throw std::runtime_error(entry->state.message);
    commit(entry->state, selected);
    return entry->state;
  } catch (const std::exception& error) {
    auto message = std::string("Could not attach folders: ") + error.what();
    try {
      impl_->WriteFolders(*entry, original);
      impl_->Rediscover(*entry);
      if (impl_->options.process->Guest() && entry->pid &&
          impl_->Plan(*entry).mounts != entry->mounted)
        impl_->Stop(*entry);
      if (!was_ready && entry->pid) impl_->Stop(*entry);
      if (was_ready) {
        impl_->Start(*entry, start);
        if (entry->state.phase != H::EnvironmentPhase::Ready)
          throw std::runtime_error(entry->state.message);
      }
      impl_->Notify(*entry);
    } catch (const std::exception& recovery) {
      message += "; could not restore the environment: " +
                 std::string(recovery.what());
    }
    throw std::runtime_error(message);
  }
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
      if (entry->state.phase != H::EnvironmentPhase::Ready) entry->requirements.reset();
      impl_->Start(*entry, request);
      break;
    case H::EnvironmentAction::Stop:
      impl_->Stop(*entry);
      break;
    case H::EnvironmentAction::AddFolders:
    case H::EnvironmentAction::RemoveFolders: {
      PrivateDirectory(
          std::filesystem::path(entry->state.control_dir).parent_path());
      PrivateDirectory(entry->state.control_dir);
      const auto previous = entry->mounted.empty() ? impl_->Plan(*entry).mounts : entry->mounted;
      if (entry->state.phase == H::EnvironmentPhase::Ready)
        entry->requirements = impl_->options.process->Requirements(entry->state);
      auto folders = entry->folders;
      for (const auto& folder : request.folders) {
        const auto found = std::find(folders.begin(), folders.end(), folder);
        if (request.action == H::EnvironmentAction::RemoveFolders) {
          if (found != folders.end()) folders.erase(found);
        } else if (found == folders.end())
          folders.push_back(folder);
      }
      impl_->WriteFolders(*entry, folders);
      // A running guest sees the folders of its start (see Start).
      if (impl_->options.process->Guest() &&
          entry->state.phase == H::EnvironmentPhase::Ready &&
          impl_->Plan(*entry).mounts != previous) {
        impl_->Stop(*entry);
        H::EnvironmentRequestT restart = request;
        restart.origins = entry->origins;
        impl_->Start(*entry, restart);
      }
      impl_->Notify(*entry);
      break;
    }
    case H::EnvironmentAction::State:
      if (!impl_->CheckDeath(*entry)) {
        if (!entry->pid) {
          if (impl_->ReadReady(*entry)) {
            entry->requirements = impl_->options.process->Requirements(entry->state);
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
  entry->state.folders = entry->folders;
  return entry->state;
}
}  // namespace tsugumi::host
