#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "server.hh"

namespace tsugumi::agent {

struct HostOptions {
  std::filesystem::path state_dir;
  std::filesystem::path demos_dir;
  std::string user_token;
  std::string external_token;
  std::string server_id;
};

class Host final : public RequestSink {
public:
  explicit Host(HostOptions options);
  ~Host() override;
  auto Attach(Server *server, Server *host_server) -> void;
  auto Shutdown() -> void;
  auto hello(const RequestIn &request) -> std::pair<std::string, bool> override;
  auto dispatch(RequestIn request) -> void override;
  auto disconnected(ConnId connection) -> void override;

private:
  struct State;
  std::unique_ptr<State> state_;
};

auto RunHost(int argc, char **argv) -> int;

} // namespace tsugumi::agent
