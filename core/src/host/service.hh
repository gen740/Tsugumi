#pragma once

#include <memory>
#include <string>
#include <utility>

#include "server.hh"

namespace tsugumi::agent {
struct HostOptions;
}
namespace tsugumi::host {
// Owns component lifetime, shared-gate wiring and both authenticated dispatch
// paths.
class Service final : public RequestSink {
 public:
  explicit Service(agent::HostOptions options);
  ~Service() override;
  auto Attach(Server* server, Server* host_server) -> void;
  auto Shutdown() -> void;
  auto hello(const RequestIn& request) -> std::pair<std::string, bool> override;
  auto dispatch(RequestIn request) -> void override;
  auto disconnected(ConnId connection) -> void override;
  auto host_disconnected(ConnId connection) -> void;

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace tsugumi::host
