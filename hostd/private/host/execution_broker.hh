#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {

// The host RPC is the only management surface. Project daemons get a scoped
// capability through their private control share, never worker credentials.
class ExecutionBroker {
 public:
  ExecutionBroker(std::filesystem::path state, std::string socket);
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
  auto endpoint() const -> std::string;
  auto Stop() -> void;

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace tsugumi::host
