// An external tsugumid double: real readiness files and authenticated project
// RPC, with startup failure and crash controls. It never invokes Nix.
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

#include "server.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json_object.hh"

namespace {
namespace R = Tsugumi::RPC;
namespace json = tsugumi::json;
class Daemon final : public tsugumi::RequestSink {
 public:
  std::string token = tsugumi::crypto::random_token(16);
  std::string instance = tsugumi::crypto::uuid4();
  std::string project;
  tsugumi::Server* server = nullptr;
  auto hello(const tsugumi::RequestIn& request)
      -> std::pair<std::string, bool> override {
    const auto* input =
        project_request(request)->value_as_SessionHelloRequest();
    const bool accepted = input && input->token() &&
                          input->token()->str() == token &&
                          input->expected_project_id() &&
                          input->expected_project_id()->str() == project;
    flatbuffers::FlatBufferBuilder b;
    if (!accepted) return {"", false};
    std::vector<flatbuffers::Offset<flatbuffers::String>> capabilities;
    auto hello = R::CreateHelloResult(b, b.CreateString(project),
                                      b.CreateString(instance),
                                      b.CreateVector(capabilities));
    auto reply = R::CreateReply(b, R::ReplyPayload::HelloResult, hello.Union());
    R::FinishMessageBuffer(
        b, R::CreateMessage(b, request.request_id, R::Payload::Reply,
                            reply.Union()));
    return {{reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()},
            true};
  }
  auto dispatch(tsugumi::RequestIn request) -> void override {
    if (!project_request(request)->value_as_ProjectHostRequirementsRequest())
      return;
    flatbuffers::FlatBufferBuilder b;
    auto value = R::CreateProjectHostRequirements(b);
    auto reply = R::CreateReply(b, R::ReplyPayload::ProjectHostRequirements,
                                value.Union());
    R::FinishMessageBuffer(
        b, R::CreateMessage(b, request.request_id, R::Payload::Reply,
                            reply.Union()));
    server->reply(
        request.conn,
        {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()});
  }
  auto disconnected(tsugumi::ConnId) -> void override {}
};
}  // namespace

auto main(int argc, char** argv) -> int {
  namespace fs = tsugumi::fs;
  std::filesystem::path project, control;
  std::string unix_socket;
  std::vector<std::string> origins;
  for (int i = 1; i < argc; ++i) {
    const std::string option(argv[i]);
    if (++i == argc) return 2;
    const std::string value(argv[i]);
    if (option == "--project") project = value;
    if (option == "--control-dir") control = value;
    if (option == "--unix") unix_socket = value;
    if (option == "--allow-origin") origins.push_back(json::Quote(value));
  }
  const auto record = json::Encode(json::Members{
      {"origins", json::Encode(origins)},
      {"broker", json::Quote(std::getenv("TSUGUMI_EXECUTION_BROKER")
                                 ? std::getenv("TSUGUMI_EXECUTION_BROKER")
                                 : "")},
      {"capability",
       json::Quote(std::getenv("TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE")
                       ? std::getenv("TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE")
                       : "")}});
  {
    std::ofstream launches(project / "launches", std::ios::app);
    launches << record << '\n';
  }
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGTERM);
  sigaddset(&signals, SIGINT);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  signal(SIGPIPE, SIG_IGN);
  Daemon daemon;
  daemon.project = fs::read_file(project / ".tsugumi/project-id")
                       .value_or(tsugumi::crypto::short_id());
  fs::write_atomic(project / ".tsugumi/project-id", daemon.project, 0600);
  tsugumi::ServerOptions options;
  options.unix_path = unix_socket;
  options.tcp_host = "127.0.0.1";
  options.tcp_port = 0;
  tsugumi::ProjectEndpoint(options);
  tsugumi::ProjectEndpoint(options);
  tsugumi::Server server(options, &daemon);
  daemon.server = &server;
  server.start();
  fs::write_atomic(
      control / "server.ready",
      json::Encode(json::Members{
          {"protocol", "1"},
          {"pid", std::to_string(getpid())},
          {"endpoint",
           json::Quote("ws://127.0.0.1:" +
                       std::to_string(server.bound_tcp_port()) + "/rpc")},
          {"token", json::Quote(daemon.token)},
          {"project_id", json::Quote(daemon.project)},
          {"server_instance_id", json::Quote(daemon.instance)},
          {"unix_socket", json::Quote(unix_socket)}}),
      0600);
  int caught = 0;
  sigwait(&signals, &caught);
  std::filesystem::remove(control / "server.ready");
  server.stop();
  return 0;
}
