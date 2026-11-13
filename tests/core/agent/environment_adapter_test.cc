// The narrow process/helper adapter cannot register a project, archive history,
// or decide when an environment becomes ready; those decisions stay in core.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>

#include "../agent_fixture_server.hh"
#include "host/environment.hh"

namespace tsugumi::test {
namespace {
namespace H = Tsugumi::Host;
class HelperDouble final : public host::EnvironmentProcess {
 public:
  std::atomic<bool> running = false;
  std::atomic<int> launches = 0;
  std::atomic<int> signals = 0;
  bool fail = false;
  bool guest = false;
  host::EnvironmentLaunch shares;
  auto Guest() const -> bool override { return guest; }
  auto Shares(const host::EnvironmentLaunch& value) -> void override {
    shares = value;
  }
  host::EnvironmentLaunch received;
  auto Launch(const host::EnvironmentLaunch& launch) -> int64_t override {
    ++launches;
    received = launch;
    if (fail) throw std::runtime_error("helper could not run the process");
    const auto control = std::find(launch.arguments.begin(),
                                   launch.arguments.end(), "--control-dir");
    if (control == launch.arguments.end())
      throw std::runtime_error("missing control");
    Write(
        std::filesystem::path(*std::next(control)) / "server.ready",
        R"({"protocol":1,"pid":42,"endpoint":"ws://localhost:1/rpc","token":"fixture","project_id":"project","server_instance_id":"instance","unix_socket":"fixture"})");
    running = true;
    return 42;
  }
  auto Poll(int64_t, bool) -> std::optional<int> override {
    return running ? std::nullopt : std::optional<int>(0);
  }
  auto Signal(int64_t, int) -> void override {
    ++signals;
    running = false;
  }
  auto Verify(const H::EnvironmentStateT& ready) -> bool override {
    return running && ready.token == "fixture";
  }
};
struct AuthorizationDouble {
  std::atomic<bool> active = false;
  auto Bind() -> host::EnvironmentAuthorization {
    return {[this](const std::string&, const std::filesystem::path&,
                   const std::filesystem::path& control) {
              active = true;
              H::ExecutionAuthorizationT grant;
              grant.available = true;
              grant.endpoint = "unix:fixture";
              grant.capability_file =
                  (control / "execution-broker-capability").string();
              Write(grant.capability_file, "fixture-capability");
              return grant;
            },
            [this](const std::string&) { active = false; }};
  }
};
TEST(EnvironmentAdapterTest, CoreOwnsReadinessAndKeepsTheProcessAfterShutdown) {
  TemporaryDirectory directory;
  Write(directory.path / "project/tsugumi.json", "{}");
  AuthorizationDouble authorization;
  auto helper = std::make_shared<HelperDouble>();
  host::EnvironmentOptions options;
  options.directory = directory.path / "environments";
  options.socket_directory = directory.path / "sockets";
  options.process = helper;
  std::atomic<int> registrations = 0;
  host::Environments service(
      options, authorization.Bind(),
      [&](const auto& state) {
        if (state.phase == H::EnvironmentPhase::Ready) ++registrations;
      },
      [](const auto&) {});
  H::EnvironmentRequestT input;
  input.project = (directory.path / "project").string();
  input.action = H::EnvironmentAction::Start;
  auto ready = service.Handle(input);
  EXPECT_EQ(ready.phase, H::EnvironmentPhase::Ready);
  EXPECT_TRUE(helper->received.authorization.available);
  EXPECT_FALSE(Read(helper->received.authorization.capability_file).empty());
  EXPECT_GE(registrations.load(), 1);
  EXPECT_EQ(service.Handle(input).pid, 42);
  EXPECT_EQ(helper->launches.load(), 1);
  service.Shutdown();
  EXPECT_TRUE(helper->running);
  EXPECT_TRUE(authorization.active);
  EXPECT_EQ(helper->signals.load(), 0);
}
TEST(EnvironmentAdapterTest, HelperFailureRevokesAuthorizationAndCanBeRetried) {
  TemporaryDirectory directory;
  Write(directory.path / "project/tsugumi.json", "{}");
  AuthorizationDouble authorization;
  auto helper = std::make_shared<HelperDouble>();
  helper->fail = true;
  host::EnvironmentOptions options;
  options.directory = directory.path / "environments";
  options.socket_directory = directory.path / "sockets";
  options.process = helper;
  host::Environments service(
      options, authorization.Bind(), [](const auto&) {}, [](const auto&) {});
  H::EnvironmentRequestT input;
  input.project = (directory.path / "project").string();
  input.action = H::EnvironmentAction::Start;
  auto failed = service.Handle(input);
  EXPECT_EQ(failed.phase, H::EnvironmentPhase::Failed);
  EXPECT_FALSE(authorization.active);
  EXPECT_EQ(failed.message, "helper could not run the process");
  EXPECT_FALSE(
      std::filesystem::exists(std::filesystem::path(failed.control_dir) /
                              "execution-broker-capability"));
  helper->fail = false;
  EXPECT_EQ(service.Handle(input).phase, H::EnvironmentPhase::Ready);
  input.action = H::EnvironmentAction::Stop;
  EXPECT_EQ(service.Handle(input).phase, H::EnvironmentPhase::Stopped);
  EXPECT_EQ(helper->signals.load(), 1);
  EXPECT_FALSE(authorization.active);
}
TEST(EnvironmentAdapterTest, CorePlansMountsAndRestartsOnlyForFolderAdditions) {
  TemporaryDirectory directory;
  const auto project = directory.path / "project",
             data = directory.path / "data", chat = directory.path / "chat";
  std::filesystem::create_directory(data);
  std::filesystem::create_directory(chat);
  Write(project / "tsugumi.json", "{}");
  Write(project / "definitions/a/_card.json",
        "{\"hostFolders\":[{\"path\":" + Quote(data.string()) + "}]}");
  Write(project / "externals/x/definitions/b/_card.json",
        "{\"hostFolders\":[{\"path\":" + Quote(data.string()) +
            ",\"write\":true}]}");
  AuthorizationDouble authorization;
  auto helper = std::make_shared<HelperDouble>();
  helper->guest = true;
  host::EnvironmentOptions options;
  options.directory = directory.path / "environments";
  options.socket_directory = directory.path / "sockets";
  options.process = helper;
  host::Environments service(
      options, authorization.Bind(), [](const auto&) {}, [](const auto&) {});
  H::EnvironmentRequestT input;
  input.project = project.string();
  input.action = H::EnvironmentAction::Start;
  input.folders = {chat.string()};
  ASSERT_EQ(service.Handle(input).phase, H::EnvironmentPhase::Ready);
  EXPECT_FALSE(helper->received.archive);
  const auto& mounts = helper->received.mounts;
  ASSERT_EQ(mounts.size(), 4u);
  EXPECT_EQ(mounts[2].path, data.string());
  EXPECT_FALSE(mounts[2].read_only);
  EXPECT_EQ(mounts[3].path, chat.string());
  EXPECT_TRUE(mounts[3].read_only);
  input.action = H::EnvironmentAction::Folders;
  input.folders.clear();
  service.Handle(input);
  EXPECT_EQ(helper->shares.mounts.size(), 3u);
  EXPECT_EQ(helper->launches, 1);
  input.folders = {chat.string()};
  service.Handle(input);
  EXPECT_EQ(helper->launches, 2);
  EXPECT_EQ(helper->signals, 1);
  input.action = H::EnvironmentAction::Stop;
  service.Handle(input);
}
}  // namespace
}  // namespace tsugumi::test
