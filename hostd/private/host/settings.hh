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
// API-key storage has a narrow interface, independent of platform UI and OAuth.
class SecretStore {
 public:
  virtual ~SecretStore() = default;
  virtual auto Get(const std::string& key) -> std::string = 0;
  virtual auto Put(const std::string& key, const std::string& secret)
      -> void = 0;
  virtual auto Erase(const std::string& key) -> void = 0;
};
// The shared VM's size on this machine. Defaults: half the memory (4-16 GiB),
// all but two cores (2-8), a 64 GiB store. Limits: every core, all but 2 GiB
// of memory, a 2 TiB store. Settings outside them are refused.
auto VMDefaultResources() -> Tsugumi::Host::VMResourcesT;
auto VMResourceLimits() -> Tsugumi::Host::VMResourcesT;
class Settings {
 public:
  using Import =
      std::function<void(const std::string&, const ::tsugumi::json::Members&)>;
  using Token =
      std::function<std::string(const std::string&, std::string_view)>;
  using Forget = std::function<bool(const std::string&)>;
  using Models = std::function<std::vector<std::string>()>;
  enum class KeyStorage { Host, Keychain };
#ifdef __APPLE__
  static constexpr auto kKeyStorage = KeyStorage::Keychain;
#else
  static constexpr auto kKeyStorage = KeyStorage::Host;
#endif
  Settings(const std::filesystem::path& directory, Import import, Token token,
           Forget forget, std::unique_ptr<SecretStore> secrets = {},
           KeyStorage storage = kKeyStorage, Models models = {},
           std::function<void(const std::string&)> provider_changed = {});
  ~Settings();
  static auto Handles(Tsugumi::Host::RequestPayload type) -> bool;
  auto Handle(const Tsugumi::Host::Request& request, uint64_t id)
      -> std::string;
  auto Redact(std::string text) -> std::string;
  auto AllowUnscopedHelpers(const std::string& profile) -> bool;
  auto TrustedFolders() -> std::vector<std::string>;
  // vm.cpus, vm.memoryGiB and vm.storeGiB of the host profile; 0 (the default)
  // takes VMDefaultResources' value.
  auto VMResources() -> Tsugumi::Host::VMResourcesT;
  auto Language(const std::string& profile) -> std::string;
  auto ModelSelection(const std::string& profile, const std::string& purpose)
      -> Tsugumi::Service::ProviderSelectionT;
  auto Resolve(const ::tsugumi::json::Members& selection)
      -> agent::ProviderConfig;

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace tsugumi::host
