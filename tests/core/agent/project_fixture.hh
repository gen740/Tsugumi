#pragma once

#include "fixture.hh"

namespace tsugumi::test {

// Project integration is opt-in; Agent-only scenarios never start a core
// project.
class AgentProjectFixture : public AgentFixture {
 protected:
  auto TearDown() -> void override {
    AgentFixture::TearDown();
    project.Stop();
  }

  auto StartProject(bool shell = false) -> void {
    Write(
        directory.path / "project/tsugumi.json",
        R"({"name":"PRIVATE_PROJECT_SENTINEL","pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})");
    Write(directory.path / "project/layout.json", "{}");
    std::vector<std::string> args{TSUGUMI_PROJECT_BINARY,
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
    if (shell) args.push_back("--project-shell");
    project.Start(args, directory.path / "project.log");
    project_ready =
        Readiness(project, directory.path / "project-control/server.ready",
                  directory.path / "project.log");
    project_id = JsonString(project_ready, "/project_id");
    auto reply = Call(
        *human, A::RequestPayload::RegisterProjectRequest, [&](Builder& b) {
          return A::CreateRegisterProjectRequest(
                     b, b.CreateString(project_id),
                     b.CreateString("unix:" +
                                    JsonString(project_ready, "/unix_socket")),
                     b.CreateString(JsonString(project_ready, "/token")),
                     b.CreateString("Private project"))
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
          R"({"template":"draft"})");
    Write(directory.path / "project/definitions/picture/src/pixel.png",
          std::string_view(pixel, sizeof(pixel) - 1 - (complete ? 0 : 8)));
    StartProject();
  }

  ChildProcess project;
  std::string project_ready;
};

}  // namespace tsugumi::test
