#pragma once

#include "fixture.hh"

namespace tsugumi::test {

// Project integration is opt-in; Agent-only scenarios never start a core
// project.
class AgentProjectFixture : public AgentFixture {
 protected:
  auto ApprovedTool(const std::string& conversation, const std::string& name,
                    const std::string& arguments) -> Reply {
    const auto id = "approved-" + name;
    const auto pending = Tool(conversation, name, arguments, id);
    if (pending.Code() != "grant_required" || pending.RequestId().empty())
      throw std::runtime_error("expected a per-call approval for " + name);
    const auto resolved =
        Call(*human, A::RequestPayload::AccessResolveRequest, [&](Builder& b) {
          return A::CreateAccessResolveRequest(
                     b, b.CreateString(pending.RequestId()), true)
              .Union();
        });
    if (!resolved.Code().empty()) throw std::runtime_error(resolved.Code());
    return Tool(conversation, name, arguments, id);
  }

  auto Delegate(const std::string& conversation, const std::string& arguments,
                const std::string& operation) -> Reply {
    // This call runs all descendants and several isolated commands. The
    // ordinary fixture's five-second RPC budget is for a single operation.
    auto result =
        external
            ->Call(
                [&](uint64_t id) {
                  Builder b;
                  const auto value = A::CreateToolCallRequest(
                      b, b.CreateString(conversation),
                      b.CreateString(project_id), b.CreateString("delegate"),
                      b.CreateString(arguments), b.CreateString(operation));
                  const auto request = A::CreateRequest(
                      b, A::RequestPayload::ToolCallRequest, value.Union());
                  A::FinishMessageBuffer(
                      b, A::CreateMessage(b, id, A::Payload::Request,
                                          request.Union()));
                  return client::Bytes(b.GetBufferPointer(),
                                       b.GetBufferPointer() + b.GetSize());
                },
                30s)
            .get();
    if (result.error) throw std::runtime_error(result.error.message);
    return {std::move(result.bytes)};
  }

  auto TearDown() -> void override {
    AgentFixture::TearDown();
    project.Stop();
  }

  auto StartProject(
      bool shell = false,
      const std::string& document =
          R"({"name":"PRIVATE_PROJECT_SENTINEL","pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})",
      const std::string& layout = "{}") -> void {
    Write(directory.path / "project/tsugumi.json", document);
    Write(directory.path / "project/layout.json", layout);
    std::vector<std::string> args{
        "/usr/bin/env",
        "TSUGUMI_MOCK_CONTROL=" + (directory.path / "mock").string(),
        TSUGUMI_PROJECT_BINARY,
        "--scratch-dir",
        (directory.path / "scratch").string(),
        "--run-scratch-dir",
        (directory.path / "run-scratch").string(),
        "--project",
        (directory.path / "project").string(),
        "--unix",
        (directory.path / "project.sock").string(),
        "--listen",
        "127.0.0.1:0",
        "--control-dir",
        (directory.path / "project-control").string(),
        "--worker",
        TSUGUMI_WORKER_BINARY,
        "--sdk-dir",
        TSUGUMI_TEST_SDK};
    if (!project_fault.empty())
      args.insert(args.begin() + 2, {"TSUGUMI_ENABLE_FAULTS=1",
                                     "TSUGUMI_FAULT=" + project_fault});
    if (shell) args.push_back("--project-shell");
    project.Start(args, directory.path / "project.log");
    project_ready =
        Readiness(project, directory.path / "project-control/server.ready",
                  directory.path / "project.log");
    project_id = JsonString(project_ready, "/project_id");
    auto reply =
        Call(*host, H::RequestPayload::RegisterProjectRequest, [&](Builder& b) {
          return H::CreateRegisterProjectRequest(
                     b, b.CreateString(project_id),
                     b.CreateString("unix:" +
                                    JsonString(project_ready, "/unix_socket")),
                     b.CreateString(JsonString(project_ready, "/token")),
                     b.CreateString("Private project"),
                     b.CreateString((directory.path / "project").string()))
              .Union();
        });
    if (!reply.Code().empty())
      throw std::runtime_error("cannot register test project: " + reply.Code());
  }

  auto StartImageProject(bool complete) -> void {
    static constexpr char pixel[] =
        "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52"
        "\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89"
        "\x00\x00\x00\x0d\x49\x44\x41\x54\x78\x9c\x63\xf8\xcf\xc0\xf0\x1f"
        "\x00\x05\x00\x01\xff\x89\x99\x3d\x1d\x00\x00\x00\x00\x49\x45\x4e\x44"
        "\xae\x42\x60\x82";
    Write(directory.path / "project/definitions/picture/_card.json",
          R"({"type":"draft"})");
    Write(directory.path / "project/definitions/picture/src/pixel.png",
          std::string_view(pixel, sizeof(pixel) - 1 - (complete ? 0 : 8)));
    StartProject();
  }

  std::string project_fault;
  ChildProcess project;
  std::string project_ready;
};

}  // namespace tsugumi::test
