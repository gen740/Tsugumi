// The narrow process/helper adapter cannot register a project, archive history,
// or decide when an environment becomes ready; those decisions stay in core.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <utility>

#include "../agent_fixture_server.hh"
#include "host/environment.hh"
#include "host_requirements.hh"

namespace tsugumi::test {
namespace {
namespace H = Tsugumi::Host;
class HelperDouble final : public host::EnvironmentProcess {
 public:
  std::atomic<bool> running = false;
  std::atomic<int> launches = 0;
  std::atomic<int> signals = 0;
  bool fail = false;
  bool fail_once = false;
  bool guest = false;
  bool verify = true;
  bool die = false;
  bool change_requirements = false;
  std::string log;
  auto Guest() const -> bool override { return guest; }
  Tsugumi::RPC::ProjectHostRequirementsT accepted;
  auto Requirements(const H::EnvironmentStateT&) -> Tsugumi::RPC::ProjectHostRequirementsT override {
    return accepted;
  }
  host::EnvironmentLaunch received;
  auto Launch(const host::EnvironmentLaunch& launch) -> int64_t override {
    ++launches;
    received = launch;
    if (guest) {
      accepted = load_host_requirements(launch.project);
      if (change_requirements && !accepted.folders.empty()) accepted.folders.front()->write = true;
    }
    if (fail || std::exchange(fail_once, false))
      throw std::runtime_error("helper could not run the process");
    const auto control = std::find(launch.arguments.begin(),
                                   launch.arguments.end(), "--control-dir");
    if (control == launch.arguments.end())
      throw std::runtime_error("missing control");
    Write(
        std::filesystem::path(*std::next(control)) / "server.ready",
        R"({"protocol":1,"pid":42,"endpoint":"ws://localhost:1/rpc","token":"fixture","project_id":"project","server_instance_id":"instance","unix_socket":"fixture"})");
    Write(launch.log, log);
    running = !die;
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
    return running && verify && ready.token == "fixture";
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
  options.run_scratch_directory = directory.path / "run-scratch";
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
  for (const auto* option : {"--run-scratch-dir", "--scratch-dir"}) {
    const auto found = std::find(helper->received.arguments.begin(), helper->received.arguments.end(), option);
    ASSERT_NE(found, helper->received.arguments.end());
    ASSERT_NE(std::next(found), helper->received.arguments.end());
    const auto relative = std::filesystem::path(*std::next(found)).lexically_relative(directory.path);
    EXPECT_FALSE(relative.empty());
    EXPECT_NE(*relative.begin(), "..");
  }
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
  options.run_scratch_directory = directory.path / "run-scratch";
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
TEST(EnvironmentAdapterTest,
     FailureAndTimeoutIncludeBoundedRedactedDaemonTail) {
  for (const bool die : {false, true}) {
    SCOPED_TRACE(die ? "exit" : "timeout");
    TemporaryDirectory directory;
    Write(directory.path / "project/tsugumi.json", "{}");
    AuthorizationDouble authorization;
    auto helper = std::make_shared<HelperDouble>();
    helper->verify = false;
    helper->die = die;
    // Credentials at both tail boundaries must not be partially exposed.
    helper->log = "discarded beginning\n" + std::string(70000, 'x') +
                  "fixture fixture-capability\nfinal stderr diagnosis\n";
    helper->log.replace(helper->log.size() - 65536 - 3, 7, "fixture");
    helper->log.replace(helper->log.size() - 4096 - 3, 18,
                        "fixture-capability");
    host::EnvironmentOptions options;
    options.directory = directory.path / "environments";
    options.socket_directory = directory.path / "sockets";
    options.run_scratch_directory = directory.path / "run-scratch";
    options.start_timeout = std::chrono::milliseconds(1);
    options.process = helper;
    host::Environments service(
        options, authorization.Bind(), [](const auto&) {}, [](const auto&) {});
    H::EnvironmentRequestT input;
    input.project = (directory.path / "project").string();
    input.action = H::EnvironmentAction::Start;
    const auto failed = service.Handle(input);
    EXPECT_EQ(failed.phase, H::EnvironmentPhase::Failed);
    EXPECT_NE(
        failed.message.find(die ? "tsugumid exited" : "did not become ready"),
        std::string::npos);
    EXPECT_NE(failed.message.find("final stderr diagnosis"), std::string::npos);
    EXPECT_NE(failed.message.find("*******"), std::string::npos);
    EXPECT_EQ(failed.message.find("discarded beginning"), std::string::npos);
    EXPECT_EQ(failed.message.find("fixture"), std::string::npos);
    EXPECT_EQ(failed.log.find("fixture"), std::string::npos);
    EXPECT_EQ(failed.log.find("ture"), std::string::npos);
    EXPECT_EQ(failed.message.find("capability"), std::string::npos);
    EXPECT_LE(failed.message.size(), 4200u);
    EXPECT_LE(failed.log.size(), 65536u);
    EXPECT_FALSE(authorization.active);
    EXPECT_FALSE(helper->running);
  }
}
TEST(EnvironmentAdapterTest, HostUsesCoreRequirementsAndDoesNotRescanUnacceptedFiles) {
  TemporaryDirectory directory;
  const auto project = directory.path / "project",
             data = directory.path / "data", chat = directory.path / "chat";
  std::filesystem::create_directory(data);
  std::filesystem::create_directory(chat);
  Write(project / "tsugumi.json",
        R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"a"},{"id":"nested"}],"connections":[]}})");
  Write(project / "definitions/a/_card.json",
        "{\"hostFolders\":[{\"name\":\"data\",\"path\":" + Quote(data.string()) + "}]}");
  Write(project / "definitions/nested/_link.json", R"({"external":"x"})");
  Write(project / "externals/x/tsugumi.json",
        R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"b"}],"connections":[]}})");
  Write(project / "externals/x/definitions/b/_card.json",
        "{\"hostFolders\":[{\"name\":\"data\",\"path\":" + Quote(data.string()) + ",\"write\":true}]}");
  Write(project / "definitions/unused/_card.json",
        "{\"hostFolders\":[{\"name\":\"chat\",\"path\":" + Quote(chat.string()) + ",\"write\":true}]}");
  Write(project / "externals/unused/definitions/ignored/_card.json",
        "{\"hostFolders\":[{\"name\":\"ignored\",\"path\":" + Quote(chat.string()) + ",\"write\":true}]}");
  AuthorizationDouble authorization;
  auto helper = std::make_shared<HelperDouble>();
  helper->guest = true;
  host::EnvironmentOptions options;
  options.directory = directory.path / "environments";
  options.socket_directory = directory.path / "sockets";
  options.run_scratch_directory = directory.path / "run-scratch";
  options.process = helper;
  host::Environments service(
      options, authorization.Bind(), [](const auto&) {}, [](const auto&) {});
  H::EnvironmentRequestT input;
  input.project = project.string();
  input.action = H::EnvironmentAction::AddFolders;
  input.folders = {chat.string()};
  service.Handle(input);
  input.action = H::EnvironmentAction::Start;
  input.folders.clear();
  const auto ready = service.Handle(input);
  ASSERT_EQ(ready.phase, H::EnvironmentPhase::Ready) << ready.message;
  EXPECT_FALSE(helper->received.archive);
  ASSERT_EQ(helper->accepted.folders.size(), 2u);
  EXPECT_EQ(helper->accepted.folders[0]->card->segments, (CardPath{"a"}));
  EXPECT_EQ(helper->accepted.folders[1]->card->segments, (CardPath{"nested", "b"}));
  const auto& mounts = helper->received.mounts;
  ASSERT_EQ(mounts.size(), 4u);
  EXPECT_EQ(mounts[2].path, data.string());
  EXPECT_FALSE(mounts[2].read_only);
  EXPECT_EQ(mounts[3].path, chat.string());
  EXPECT_TRUE(mounts[3].read_only);
  // A disk edit not accepted by the running core must not alter its mounts,
  // and a start of a ready project leaves the running guest's folders alone.
  Write(project / "externals/x/definitions/b/_card.json", "unaccepted broken JSON");
  EXPECT_EQ(service.Handle(input).phase, H::EnvironmentPhase::Ready);
  EXPECT_EQ(helper->launches, 1);
  EXPECT_EQ(helper->signals, 0);
  Write(project / "externals/x/definitions/b/_card.json",
        "{\"hostFolders\":[{\"name\":\"data\",\"path\":" + Quote(data.string()) + ",\"write\":true}]}");
  // A guest keeps the folders of its start: any other set starts it again.
  input.action = H::EnvironmentAction::RemoveFolders;
  input.folders = {chat.string()};
  service.Handle(input);
  EXPECT_EQ(helper->launches, 2);
  EXPECT_EQ(helper->signals, 1);
  ASSERT_EQ(helper->received.mounts.size(), 3u);
  EXPECT_FALSE(helper->received.mounts[2].read_only);
  input.action = H::EnvironmentAction::AddFolders;
  service.Handle(input);
  EXPECT_EQ(helper->launches, 3);
  EXPECT_EQ(helper->signals, 2);
  ASSERT_EQ(helper->received.mounts.size(), 4u);
  input.action = H::EnvironmentAction::Stop;
  service.Handle(input);
}
TEST(EnvironmentAdapterTest, AttachWaitsForTheGuestAndRollsBackFailedBatches) {
  TemporaryDirectory directory;
  const auto project = directory.path / "project";
  Write(
      project / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})");
  auto helper = std::make_shared<HelperDouble>();
  helper->guest = true;
  AuthorizationDouble authorization;
  host::EnvironmentOptions options;
  options.directory = directory.path / "environments";
  options.socket_directory = directory.path / "sockets";
  options.process = helper;
  host::Environments service(
      options, authorization.Bind(), [](const auto&) {}, [](const auto&) {});
  H::EnvironmentRequestT start;
  start.project = project.string();
  start.action = H::EnvironmentAction::Start;
  ASSERT_EQ(service.Handle(start).phase, H::EnvironmentPhase::Ready);
  std::vector<std::string> registered;
  const auto commit = [&](const auto& state, const auto& folders) {
    EXPECT_EQ(state.phase, H::EnvironmentPhase::Ready);
    EXPECT_TRUE(helper->running);
    EXPECT_EQ(helper->received.folders, state.folders);
    registered = folders;
  };
  H::AttachFoldersRequestT attach;
  attach.project = project.string();
  for (int i = 0; i < 3; ++i) {
    const auto folder = (directory.path / std::to_string(i)).string();
    std::filesystem::create_directory(folder);
    attach.folders = {folder, folder};
    const auto state = service.Attach(attach, commit);
    EXPECT_EQ(registered, std::vector<std::string>{folder});
    EXPECT_EQ(state.folders.size(), static_cast<size_t>(i + 1));
    EXPECT_EQ(helper->launches, i + 2);
    // A retry registers the existing share without restarting again.
    registered.clear();
    service.Attach(attach, commit);
    EXPECT_EQ(registered, std::vector<std::string>{folder});
    EXPECT_EQ(helper->launches, i + 2);
  }
  const auto original = service.Handle(start).folders;
  const auto extra = (directory.path / "extra").string();
  std::filesystem::create_directory(extra);
  attach.folders = {extra, (directory.path / "missing").string()};
  EXPECT_THROW(service.Attach(attach, commit), std::runtime_error);
  EXPECT_EQ(service.Handle(start).folders, original);
  EXPECT_EQ(helper->launches, 4);
  attach.folders = {extra};
  EXPECT_THROW(service.Attach(attach,
                              [](const auto&, const auto&) {
                                throw std::runtime_error("registration failed");
                              }),
               std::runtime_error);
  EXPECT_EQ(service.Handle(start).folders, original);
  EXPECT_EQ(helper->received.folders, original);
  EXPECT_TRUE(helper->running);
  // A transient restart failure restores a ready environment and old shares.
  helper->fail_once = true;
  EXPECT_THROW(service.Attach(attach, commit), std::runtime_error);
  EXPECT_TRUE(helper->running);
  EXPECT_EQ(service.Handle(start).folders, original);
  // A permanent startup failure still restores the persisted folder set.
  helper->fail = true;
  EXPECT_THROW(service.Attach(attach, commit), std::runtime_error);
  helper->fail = false;
  EXPECT_EQ(service.Handle(start).folders, original);
  service.Attach(attach, commit);
  EXPECT_EQ(registered, std::vector<std::string>{extra});
  start.action = H::EnvironmentAction::Stop;
  service.Handle(start);
}

TEST(EnvironmentAdapterTest, StartupRequirementsMustMatchTheAcceptedCoreDocument) {
  TemporaryDirectory directory;
  const auto project = directory.path / "project", data = directory.path / "data";
  std::filesystem::create_directory(data);
  Write(project / "tsugumi.json",
        R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"a"}],"connections":[]}})");
  Write(project / "definitions/a/_card.json",
        "{\"hostFolders\":[{\"name\":\"data\",\"path\":" + Quote(data.string()) + "}]}");
  auto helper = std::make_shared<HelperDouble>();
  helper->guest = true;
  helper->change_requirements = true;
  AuthorizationDouble authorization;
  host::EnvironmentOptions options;
  options.directory = directory.path / "environments";
  options.socket_directory = directory.path / "sockets";
  options.run_scratch_directory = directory.path / "run-scratch";
  options.process = helper;
  host::Environments service(options, authorization.Bind(), [](const auto&) {}, [](const auto&) {});
  H::EnvironmentRequestT input;
  input.project = project.string();
  input.action = H::EnvironmentAction::Start;
  const auto state = service.Handle(input);
  EXPECT_EQ(state.phase, H::EnvironmentPhase::Failed);
  EXPECT_NE(state.message.find("requirements changed during startup"), std::string::npos);
  EXPECT_FALSE(helper->running);
  EXPECT_FALSE(authorization.active);
}

}  // namespace
}  // namespace tsugumi::test

namespace tsugumi::test {
TEST(EnvironmentAdapterTest, FolderMutationsSurviveStartsAndServiceRecreation) {
  TemporaryDirectory directory;
  const auto project = directory.path / "project";
  Write(project / "tsugumi.json", "{}");
  const auto a = (directory.path / "a").string(),
             b = (directory.path / "b").string();
  std::filesystem::create_directory(a);
  std::filesystem::create_directory(b);
  host::EnvironmentOptions options;
  options.directory = directory.path / "environments";
  options.socket_directory = directory.path / "sockets";
  options.run_scratch_directory = directory.path / "run-scratch";
  auto helper = std::make_shared<HelperDouble>();
  options.process = helper;
  AuthorizationDouble authorization;
  const auto make = [&] {
    return std::make_unique<host::Environments>(
        options, authorization.Bind(), [](const auto&) {}, [](const auto&) {});
  };
  auto service = make();
  H::EnvironmentRequestT request;
  request.project = project.string();
  request.action = H::EnvironmentAction::AddFolders;
  request.folders = {a, a};
  EXPECT_EQ(service->Handle(request).folders, std::vector<std::string>{a});
  request.folders = {b};
  EXPECT_EQ(service->Handle(request).folders, (std::vector<std::string>{a, b}));
  // An empty/new client starting the environment cannot replace either folder.
  request.action = H::EnvironmentAction::Start;
  request.folders.clear();
  EXPECT_EQ(service->Handle(request).folders, (std::vector<std::string>{a, b}));
  EXPECT_EQ(helper->received.folders, (std::vector<std::string>{a, b}));
  service.reset();
  service = make();
  request.action = H::EnvironmentAction::State;
  EXPECT_EQ(service->Handle(request).folders, (std::vector<std::string>{a, b}));
  request.action = H::EnvironmentAction::RemoveFolders;
  request.folders = {a};
  EXPECT_EQ(service->Handle(request).folders, std::vector<std::string>{b});
  request.folders = {a};
  EXPECT_EQ(service->Handle(request).folders, std::vector<std::string>{b});
  request.action = H::EnvironmentAction::Stop;
  service->Handle(request);
}
}  // namespace tsugumi::test
