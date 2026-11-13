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

namespace tsugumi::host {

struct EnvironmentFailure : std::runtime_error {
  std::string recovery, log;
  EnvironmentFailure(std::string message, std::string r, std::string l)
      : std::runtime_error(std::move(message)),
        recovery(std::move(r)),
        log(std::move(l)) {}
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
};
struct EnvironmentLaunch {
  std::string key, project, control;
  std::vector<std::string> origins, folders;
  std::vector<EnvironmentMount> mounts;
  bool archive = false;
  std::vector<std::string> arguments;
  std::filesystem::path log;
  Tsugumi::Host::ExecutionAuthorizationT authorization;
};

// The platform adapter only runs, probes and stops a process. Readiness,
// registration, permissions and the environment's lifetime belong to agentd.
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
  virtual auto Shares(const EnvironmentLaunch&) -> void {}
  virtual auto Runtime(const Tsugumi::Host::HostRuntimeRequestT&)
      -> Tsugumi::Host::HostRuntimeT;
  virtual auto Terminal(const Tsugumi::Host::HostTerminalRequestT&)
      -> Tsugumi::Host::HostTerminalT;
  virtual auto Launch(const EnvironmentLaunch& launch) -> int64_t = 0;
  // nullopt while alive; otherwise raw wait status (-1 if unavailable).
  virtual auto Poll(int64_t pid, bool child) -> std::optional<int> = 0;
  virtual auto Signal(int64_t pid, int signal) -> void = 0;
  virtual auto Verify(const Tsugumi::Host::EnvironmentStateT& ready)
      -> bool = 0;
};

auto NativeEnvironmentProcess() -> std::shared_ptr<EnvironmentProcess>;

struct EnvironmentOptions {
  std::filesystem::path directory;
  std::filesystem::path socket_directory;
  std::filesystem::path tsugumid;
  std::filesystem::path worker;
  std::filesystem::path sdk;
  std::chrono::milliseconds start_timeout{60000};
  std::chrono::milliseconds stop_timeout{20000};
  std::shared_ptr<EnvironmentProcess> process;
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
  auto Runtime(const Tsugumi::Host::HostRuntimeRequestT& request)
      -> Tsugumi::Host::HostRuntimeT;
  auto Terminal(const Tsugumi::Host::HostTerminalRequestT& request)
      -> Tsugumi::Host::HostTerminalT;
  // Stops observation and pending starts; ready processes remain resident.
  auto Shutdown() -> void;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace tsugumi::host
