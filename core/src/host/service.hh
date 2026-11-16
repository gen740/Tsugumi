#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "host/environment.hh"
#include "host/vm_process.hh"
#include "server.hh"

namespace tsugumi::host {
struct HostOptions {
  std::filesystem::path state_dir;
  std::filesystem::path demos_dir;
  std::string broker_socket;
  std::string user_token;
  std::string external_token;
  std::string server_id;
  EnvironmentOptions environment;
  VMProcessOptions vm;
};

// Owns component lifetime, shared-gate wiring and both authenticated dispatch
// paths.
class Service final : public RequestSink {
 public:
  explicit Service(HostOptions options);
  ~Service() override;
  auto Attach(Server* server, Server* host_server) -> void;
  auto Shutdown() -> void;
  auto Idle() -> bool;
  auto DrainIdle() -> void;
  auto CanDrain() -> bool;
  auto idle() -> bool override { return Idle(); }
  auto hello(const RequestIn& request) -> std::pair<std::string, bool> override;
  auto dispatch(RequestIn request) -> void override;
  auto disconnected(ConnId connection) -> void override;
  auto host_disconnected(ConnId connection) -> void;

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace tsugumi::host
