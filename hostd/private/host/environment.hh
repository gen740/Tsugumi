#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

#include "tsugumi/protocol/host_generated.h"
#include "tsugumi/protocol/tsugumi_generated.h"

namespace tsugumi::host {

struct EnvironmentFailure : std::runtime_error {
  std::string log;
  std::string code;  // no_project_slot: every VM project slot is taken
  EnvironmentFailure(std::string message, std::string log,
                     std::string code = {})
      : std::runtime_error(std::move(message)),
        log(std::move(log)),
        code(std::move(code)) {}
};

struct EnvironmentAuthorization {
  std::function<Tsugumi::Host::ExecutionAuthorizationT(
      const std::string&, const std::filesystem::path&,
      const std::filesystem::path&)>
      authorize;
  std::function<void(const std::string&)> revoke;
};

struct EnvironmentMount {
  std::string name;
  std::string path;
  bool read_only = false;
  auto operator==(const EnvironmentMount&) const -> bool = default;
};
struct EnvironmentLaunch {
  std::string key, project, control;
  std::vector<std::string> origins, folders;
  std::vector<EnvironmentMount> mounts;
  std::string host_path_bindings;
  // The VM helper has a separate, named startup environment contract.
  bool vm_helper = false;
  std::vector<std::string> arguments;
  std::filesystem::path log;
  std::stop_token stop;
  Tsugumi::Host::ExecutionAuthorizationT authorization;
};

// The platform adapter only runs, probes and stops a process. Readiness,
// registration, permissions and the environment's lifetime belong to hostd.
class EnvironmentProcess {
 public:
  virtual ~EnvironmentProcess() = default;
  virtual auto Shutdown() -> void {}
  virtual auto Guest() const -> bool { return false; }
  virtual auto Inspect(Tsugumi::Host::EnvironmentStateT& ready)
      -> std::optional<int64_t> {
    return Verify(ready) ? std::optional<int64_t>(ready.pid) : std::nullopt;
  }
  virtual auto GuardStart(const std::string&) -> void {}
  virtual auto Busy(const Tsugumi::Host::EnvironmentStateT& ready) -> bool;
  auto ClearBuildCache(const Tsugumi::Host::EnvironmentStateT& ready)
      -> Tsugumi::RPC::ProjectCacheClearResultT;
  virtual auto Requirements(const Tsugumi::Host::EnvironmentStateT& ready)
      -> Tsugumi::RPC::ProjectHostRequirementsT;
  virtual auto Runtime(const Tsugumi::Host::HostRuntimeRequestT&)
      -> Tsugumi::Host::HostRuntimeT;
  virtual auto Terminal(const Tsugumi::Host::HostTerminalRequestT&)
      -> Tsugumi::Host::HostTerminalT;
  virtual auto Launch(const EnvironmentLaunch& launch) -> int64_t = 0;
  // nullopt while alive; otherwise raw wait status (-1 if unavailable).
  virtual auto Poll(int64_t pid, bool child) -> std::optional<int> = 0;
  virtual auto Signal(int64_t pid, int signal) -> void = 0;
  virtual auto WaitForExit(int64_t pid, bool child) -> void;
  virtual auto Observe(std::stop_token stop, const std::function<void()>& check)
      -> void;
  virtual auto WaitForReady(const std::filesystem::path& file,
                            const std::string& key, int64_t pid,
                            std::stop_token stop,
                            const std::function<bool()>& done) -> void;
  virtual auto Verify(const Tsugumi::Host::EnvironmentStateT& ready)
      -> bool = 0;
};

auto LaunchNativeProcess(const EnvironmentLaunch& launch) -> int64_t;
auto WaitForNativeReadiness(const std::filesystem::path& file, int64_t pid,
                            std::stop_token stop,
                            const std::function<bool()>& done) -> void;

auto NativeEnvironmentProcess(
    std::function<void(const Tsugumi::Host::HostTerminalDataT&)> terminal = {})
    -> std::shared_ptr<EnvironmentProcess>;

struct EnvironmentOptions {
  std::filesystem::path directory;
  std::filesystem::path socket_directory;
  std::filesystem::path run_scratch_directory;
  std::filesystem::path tsugumid;
  std::filesystem::path worker;
  std::filesystem::path nix_dir;  // installed Card evaluator
  std::shared_ptr<EnvironmentProcess> process;
  std::function<Tsugumi::Host::HostPathBindingsT(const std::string&)>
      host_paths;
  std::function<std::function<void()>(const std::filesystem::path&)>
      reserve_project_identity;
  // What holds a project folder open: "page" (a window or Web UI page shows
  // it), "session" (an Agent client opened it) or "" (nothing).
  std::function<std::string(const std::string&)> holder;
};

class Environments {
 public:
  using State = Tsugumi::Host::EnvironmentStateT;
  using Changed = std::function<void(const State&)>;
  using Progress =
      std::function<void(const Tsugumi::Host::EnvironmentProgressT&)>;
  Environments(EnvironmentOptions options,
               EnvironmentAuthorization authorization, Changed changed,
               Progress progress);
  ~Environments();
  auto Handle(const Tsugumi::Host::EnvironmentRequestT& request) -> State;
  auto List() -> Tsugumi::Host::EnvironmentListT;
  auto ChangeHostPaths(const std::string& project,
                       const std::function<std::string()>& change)
      -> std::string;
  // Commit registration under the same project lock as mount changes. The
  // callback must commit its whole batch or throw without changing resources.
  auto Attach(
      const Tsugumi::Host::AttachFoldersRequestT& request,
      const std::function<void(const State&, const std::vector<std::string>&)>&
          commit) -> State;
  auto Runtime(const Tsugumi::Host::HostRuntimeRequestT& request)
      -> Tsugumi::Host::HostRuntimeT;
  auto Terminal(const Tsugumi::Host::HostTerminalRequestT& request)
      -> Tsugumi::Host::HostTerminalT;
  auto Busy() -> bool;
  // Stops the project's environment when nothing holds it and nothing runs
  // in it; returns whether it stopped.
  auto StopUnused(const std::string& project) -> bool;
  // Stops observation and pending starts, then shuts down owned runtimes.
  auto Shutdown() -> void;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace tsugumi::host
