#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "host/environment.hh"
#include "host/vm_process.hh"
#include "tsugumi/rpc/transport.hh"

namespace tsugumi::host {
class WebPages;
struct HostOptions {
  std::filesystem::path state_dir;
  std::filesystem::path demos_dir;
  std::string application = "tsugumi-hostd";
  std::filesystem::path repo_root;
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
  auto Attach(TransportServer* server, TransportServer* host_server) -> void;
  auto Shutdown() -> void;
  auto hello(const RequestIn& request) -> std::pair<std::string, bool> override;
  auto dispatch(RequestIn request) -> void override;
  auto disconnected(ConnId connection) -> void override;
  auto host_disconnected(ConnId connection) -> void;
  /** The Web UI's origin and pages (after Attach). */
  auto web() -> WebPages&;

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace tsugumi::host
