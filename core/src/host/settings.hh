#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "agent/json.hh"
#include "agent/provider.hh"
#include "tsugumi/protocol/host_generated.h"

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
class Settings {
 public:
  using Import =
      std::function<void(const std::string&, const agent::json::Members&)>;
  using Token =
      std::function<std::string(const std::string&, std::string_view)>;
  using Forget = std::function<bool(const std::string&)>;
  Settings(const std::filesystem::path& directory, Import import, Token token,
           Forget forget, std::unique_ptr<SecretStore> secrets = {});
  ~Settings();
  static auto Handles(Tsugumi::Host::RequestPayload type) -> bool;
  auto Handle(const Tsugumi::Host::Request& request, uint64_t id)
      -> std::string;
  auto Redact(std::string text) -> std::string;
  auto VersionNoteMode(const std::string& profile) -> std::string;
  auto VersionNoteProvider(const std::string& profile, const std::string& anthropic_keys,
                           const std::string& openai_keys) -> agent::ProviderConfig;
  auto Resolve(const agent::json::Members& selection) -> agent::ProviderConfig;

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace tsugumi::host
