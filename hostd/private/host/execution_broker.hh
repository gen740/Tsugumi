#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {

// The host RPC is the only management surface. Project daemons get a scoped
// capability through their private control share, never worker credentials.
class ExecutionBroker {
 public:
  // Reads the values of the named Card secrets that are set for a hostd
  // project id, from hostd's secret store. An authorized project core asks for
  // the secrets one invocation declares, just before the call; the broker
  // passes the project its capability was issued to.
  using CardSecrets =
      std::function<std::vector<std::pair<std::string, std::string>>(
          const std::string& project, const std::vector<std::string>& names)>;
  ExecutionBroker(std::filesystem::path state, std::string socket,
                  CardSecrets card_secrets = {});
  ~ExecutionBroker();
  ExecutionBroker(const ExecutionBroker&) = delete;
  auto operator=(const ExecutionBroker&) -> ExecutionBroker& = delete;
  static auto Handles(Tsugumi::Host::RequestPayload type) -> bool;
  auto Handle(const Tsugumi::Host::Request& request, uint64_t id)
      -> std::string;
  auto Authorize(const std::string& project, const std::filesystem::path& path,
                 const std::filesystem::path& control, bool guest)
      -> Tsugumi::Host::ExecutionAuthorizationT;
  auto Revoke(const std::string& project) -> void;
  auto socket_path() const -> const std::string&;
  auto Stop() -> void;

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace tsugumi::host
