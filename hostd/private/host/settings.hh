#pragma once
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agent/provider.hh"
#include "tsugumi/protocol/host_generated.h"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::host {
struct Credentials;
// The one store of host secrets, independent of platform UI and OAuth. It
// holds provider API keys ("<profile>:<provider>") and Card secrets
// ("card-secret/<project id>/<name>"); the two key forms never overlap.
class SecretStore {
 public:
  virtual ~SecretStore() = default;
  virtual auto Get(const std::string& key) -> std::string = 0;
  virtual auto Put(const std::string& key, const std::string& secret)
      -> void = 0;
  virtual auto Erase(const std::string& key) -> void = 0;
  // Stored keys that start with `prefix`, in order.
  virtual auto Keys(const std::string& prefix) -> std::vector<std::string> = 0;
};
// The shared VM's size on this machine. Defaults: half the memory (4-16 GiB),
// all but two cores (2-8), a 64 GiB store. Limits: every core, all but 2 GiB
// of memory, a 2 TiB store. Settings outside them are refused.
auto VMDefaultResources() -> Tsugumi::Host::VMResourcesT;
auto VMResourceLimits() -> Tsugumi::Host::VMResourcesT;
class Settings {
 public:
  using Models = std::function<std::vector<std::string>()>;
  Settings(const std::filesystem::path& directory, Credentials& credentials,
           std::unique_ptr<SecretStore> secrets = {}, Models models = {},
           std::function<void(std::string)> publish = {});
  ~Settings();
  static auto Handles(Tsugumi::Host::RequestPayload type) -> bool;
  auto Handle(const Tsugumi::Host::Request& request, uint64_t id)
      -> std::string;
  auto Redact(std::string text) -> std::string;
  // The values of these Card secret names that are set for one project, for
  // one invocation. Only the execution broker calls this, with the project its
  // capability authorizes; no Host or Agent request returns a value.
  auto CardSecretValues(const std::string& project,
                        const std::vector<std::string>& names)
      -> std::vector<std::pair<std::string, std::string>>;
  auto HostPaths(const std::string& project)
      -> Tsugumi::Host::HostPathBindingsT;
  auto AllowUnscopedHelpers() -> bool;
  // vm.cpus, vm.memoryGiB and vm.storeGiB of the host; 0 (the default)
  // takes VMDefaultResources' value.
  auto VMResources() -> Tsugumi::Host::VMResourcesT;
  auto Language() -> std::string;
  auto ModelSelection(const std::string& profile, const std::string& purpose)
      -> Tsugumi::Service::ProviderSelectionT;
  auto Resolve(const ::tsugumi::json::Members& selection)
      -> agent::ProviderConfig;

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace tsugumi::host
