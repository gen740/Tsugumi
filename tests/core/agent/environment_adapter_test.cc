// The narrow process/helper adapter cannot register a project, archive history,
// or decide when an environment becomes ready; those decisions stay in core.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <future>
#include <utility>

#include "../agent_fixture_server.hh"
#include "document_fixture.hh"
#include "host/environment.hh"
#include "host/templates.hh"
#include "tsugumi/document/host_requirements.hh"
#include "tsugumi/protocol/runtime_state_generated.h"
#include "tsugumi/util/flat_file.hh"

namespace tsugumi::test {
namespace {
namespace H = Tsugumi::Host;
auto HostFolderCard(const std::string& name, bool writable) -> std::string {
  return FixtureCodeCard(json::Encode(json::Members{
      {"grants", R"({"network":false,"gui":false,"hostPaths":[{"name":)" +
                     Quote(name) + R"(,"write":)" +
                     (writable ? "true" : "false") + "}]}"}}));
}
auto HostPathSettings(const std::filesystem::path& project,
                      const std::filesystem::path& data)
    -> std::function<H::HostPathBindingsT(const std::string&)> {
  Write(project / ".tsugumi/project-id", "abcdef");
  return [data = std::filesystem::canonical(data)](const std::string& id) {
    EXPECT_EQ(id, "abcdef");
    H::HostPathBindingsT settings;
    for (const auto* scope : {"", "externals/nested/"}) {
      auto binding = std::make_unique<H::HostPathBindingT>();
      binding->scope = scope;
      binding->name = "data";
      binding->path = data.string();
      settings.bindings.push_back(std::move(binding));
    }
    return settings;
  };
}
class HelperDouble final : public host::EnvironmentProcess {
 public:
  std::atomic<bool> running = false;
  std::atomic<int> launches = 0;
  std::atomic<int> signals = 0;
  bool fail = false;
  bool fail_once = false;
  // The process starts, but the helper's answer never arrives.
  bool lose_reply_once = false;
  bool guest = false;
  bool verify = true;
  bool die = false;
  bool change_requirements = false;
  std::function<void(std::stop_token)> before_launch;
  std::string log;
  auto Busy(const H::EnvironmentStateT&) -> bool override { return false; }
  auto Guest() const -> bool override { return guest; }
  Tsugumi::RPC::ProjectHostRequirementsT accepted;
  auto Requirements(const H::EnvironmentStateT&)
      -> Tsugumi::RPC::ProjectHostRequirementsT override {
    return accepted;
  }
  host::EnvironmentLaunch received;
  auto Launch(const host::EnvironmentLaunch& launch) -> int64_t override {
    ++launches;
    if (before_launch) before_launch(launch.stop);
    received = launch;
    if (guest) {
      accepted = load_host_requirements(launch.project);
      if (change_requirements && !accepted.folders.empty())
        accepted.folders.front()->write = true;
    }
    if (fail || std::exchange(fail_once, false))
      throw std::runtime_error("helper could not run the process");
    const auto control = std::find(launch.arguments.begin(),
                                   launch.arguments.end(), "--control-dir");
    if (control == launch.arguments.end())
      throw std::runtime_error("missing control");
    if (verify) {
      Tsugumi::State::ServerReadyT ready;
      ready.protocol = 1;
      ready.pid = 42;
      ready.endpoint = "ws://localhost:1/rpc";
      ready.token = "fixture";
      ready.project_id = "project";
      ready.unix_socket = "fixture";
      Write(std::filesystem::path(*std::next(control)) / "server.ready",
            flat::encode<Tsugumi::State::ServerReady>(ready));
    }
    Write(launch.log, log);
    running = !die;
    if (std::exchange(lose_reply_once, false))
      throw std::runtime_error("helper reply was lost");
    return 42;
  }
  auto GuardStart(const std::string&) -> void override {
    if (guest && running)
      throw host::EnvironmentFailure(
          "A resident project process cannot be authenticated",
          "Stop the environment and retry.", "");
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
TEST(EnvironmentAdapterTest, VerifyStoreCancelsAnInProgressStart) {
  TemporaryDirectory directory;
  Write(directory.path / "project/tsugumi.json", FixtureProjectDocument());
  AuthorizationDouble authorization;
  auto helper = std::make_shared<HelperDouble>();
  host::EnvironmentOptions options;
  options.directory = directory.path / "environments";
  options.socket_directory = directory.path / "sockets";
  options.run_scratch_directory = directory.path / "run-scratch";
  options.process = helper;
  std::promise<void> launched;
  auto ready = launched.get_future();
  std::atomic<bool> cancelled = false;
  helper->before_launch = [&](std::stop_token stop) {
    std::mutex mutex;
    std::unique_lock lock(mutex);
    std::condition_variable_any wake;
    launched.set_value();
    wake.wait_for(lock, stop, std::chrono::seconds(3), [] { return false; });
    cancelled = stop.stop_requested();
    throw std::runtime_error("Fixture start ended");
  };
  host::Environments service(
      options, authorization.Bind(), [](const auto&) {}, [](const auto&) {});
  H::EnvironmentRequestT request;
  request.project = (directory.path / "project").string();
  request.action = H::EnvironmentAction::Start;
  auto start =
      std::async(std::launch::async, [&] { return service.Handle(request); });
  ASSERT_EQ(ready.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  H::HostRuntimeRequestT verify;
  verify.action = H::RuntimeAction::VerifyStore;
  // This fixture has no VM store to verify, but must cancel startup first.
  EXPECT_THROW(service.Runtime(verify), std::runtime_error);
  EXPECT_TRUE(cancelled);
  EXPECT_NE(start.get().phase, H::EnvironmentPhase::Ready);
}

TEST(EnvironmentAdapterTest, CoreOwnsReadinessAndStopsTheProcessOnShutdown) {
  TemporaryDirectory directory;
  Write(directory.path / "project/tsugumi.json", FixtureProjectDocument());
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
    const auto found = std::find(helper->received.arguments.begin(),
                                 helper->received.arguments.end(), option);
    ASSERT_NE(found, helper->received.arguments.end());
    ASSERT_NE(std::next(found), helper->received.arguments.end());
    const auto relative = std::filesystem::path(*std::next(found))
                              .lexically_relative(directory.path);
    EXPECT_FALSE(relative.empty());
    EXPECT_NE(*relative.begin(), "..");
  }
  EXPECT_TRUE(helper->received.authorization.available);
  EXPECT_FALSE(Read(helper->received.authorization.capability_file).empty());
  EXPECT_GE(registrations.load(), 1);
  EXPECT_EQ(service.Handle(input).pid, 42);
  EXPECT_EQ(helper->launches.load(), 1);
  service.Shutdown();
  EXPECT_FALSE(helper->running);
  EXPECT_FALSE(authorization.active);
  EXPECT_EQ(helper->signals.load(), 1);
}
TEST(EnvironmentAdapterTest, HelperFailureRevokesAuthorizationAndCanBeRetried) {
  TemporaryDirectory directory;
  Write(directory.path / "project/tsugumi.json", FixtureProjectDocument());
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
  EXPECT_EQ(failed.failure_code, H::EnvironmentFailureCode::LaunchFailed);
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

TEST(EnvironmentAdapterTest, ShutdownPreservesReplacementControlFiles) {
  TemporaryDirectory directory;
  Write(directory.path / "project/tsugumi.json", FixtureProjectDocument());
  AuthorizationDouble authorization;
  auto helper = std::make_shared<HelperDouble>();
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
  const auto ready = service.Handle(input);
  ASSERT_EQ(ready.phase, H::EnvironmentPhase::Ready);
  const auto control = std::filesystem::path(ready.control_dir);
  std::filesystem::rename(control, control.string() + "-old");
  Write(control / "server.ready", "replacement readiness");
  Write(control / "execution-broker-capability", "replacement capability");

  service.Shutdown();
  EXPECT_EQ(helper->signals.load(), 0);
  EXPECT_EQ(Read(control / "server.ready"), "replacement readiness");
  EXPECT_EQ(Read(control / "execution-broker-capability"),
            "replacement capability");
}
TEST(EnvironmentAdapterTest, FailureAndTimeoutIncludeBoundedDaemonTail) {
  TemporaryDirectory directory;
  Write(directory.path / "project/tsugumi.json", FixtureProjectDocument());
  AuthorizationDouble authorization;
  auto helper = std::make_shared<HelperDouble>();
  helper->verify = false;
  helper->die = true;
  // Only the existing tail bound applies to the daemon's own diagnostics.
  helper->log = "discarded beginning fixture-capability\n" +
                std::string(70000, 'x') + "final stderr diagnosis\n";
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
  const auto failed = service.Handle(input);
  EXPECT_EQ(failed.phase, H::EnvironmentPhase::Failed);
  EXPECT_EQ(failed.failure_code, H::EnvironmentFailureCode::ProcessExit);
  EXPECT_NE(failed.message.find("final stderr diagnosis"), std::string::npos);
  EXPECT_EQ(failed.message.find("discarded beginning"), std::string::npos);
  EXPECT_TRUE(failed.endpoint.empty());
  EXPECT_TRUE(failed.token.empty());
  EXPECT_EQ(failed.message.find("fixture-capability"), std::string::npos);
  EXPECT_NE(failed.log.find("final stderr diagnosis"), std::string::npos);
  EXPECT_EQ(failed.log.find("fixture-capability"), std::string::npos);
  EXPECT_LE(failed.log.size(), 65536u);
  EXPECT_FALSE(authorization.active);
  EXPECT_FALSE(helper->running);
}
TEST(EnvironmentAdapterTest,
     HostUsesCoreRequirementsAndDoesNotRescanUnacceptedFiles) {
  TemporaryDirectory directory;
  const auto project = directory.path / "project",
             data = directory.path / "data", chat = directory.path / "chat";
  std::filesystem::create_directory(data);
  std::filesystem::create_directory(chat);
  Write(
      project / "tsugumi.json",
      FixtureProjectDocument(
          R"({"nixPath": [],"hostPaths":[{"name":"data","description":"Input data"}],"canvas":{"cards":[{"id":"a"},{"id":"nested"}],"connections":[]}})"));
  Write(project / "definitions/a/_card.json", HostFolderCard("data", false));
  Write(project / "definitions/a/README.md", host::CardReadme("a"));
  Write(project / "definitions/nested/_card.json",
        R"({"type":"subgraph","symbol":"link"})");
  Write(
      project / "externals/nested/tsugumi.json",
      FixtureProjectDocument(
          R"({"nixPath": [],"hostPaths":[{"name":"data","description":"Input data"}],"canvas":{"cards":[{"id":"b"}],"connections":[]}})"));
  Write(project / "externals/nested/definitions/b/_card.json",
        HostFolderCard("data", true));
  Write(project / "externals/nested/definitions/b/README.md",
        host::CardReadme("b"));
  Write(project / "definitions/nested/README.md", host::CardReadme("nested"));
  AuthorizationDouble authorization;
  auto helper = std::make_shared<HelperDouble>();
  helper->guest = true;
  host::EnvironmentOptions options;
  options.directory = directory.path / "environments";
  options.socket_directory = directory.path / "sockets";
  options.run_scratch_directory = directory.path / "run-scratch";
  options.process = helper;
  options.host_paths = HostPathSettings(project, data);
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
  ASSERT_EQ(helper->accepted.folders.size(), 2u);
  EXPECT_EQ(helper->accepted.folders[0]->card->segments, (CardPath{"a"}));
  EXPECT_EQ(helper->accepted.folders[1]->card->segments,
            (CardPath{"nested", "b"}));
  const auto& mounts = helper->received.mounts;
  ASSERT_EQ(mounts.size(), 4u);
  EXPECT_EQ(mounts[2].path, std::filesystem::canonical(data).string());
  EXPECT_FALSE(mounts[2].read_only);
  EXPECT_EQ(mounts[3].path, chat.string());
  EXPECT_TRUE(mounts[3].read_only);
  ASSERT_EQ(ready.attached_roots.size(), 1u);
  EXPECT_EQ(ready.attached_roots[0]->host_path, chat.string());
  EXPECT_EQ(ready.attached_roots[0]->execution_path,
            "/run/tsugumi/chat/" + mounts[3].name.substr(5));
  // A disk edit not accepted by the running core must not alter its mounts,
  // and a start of a ready project leaves the running guest's folders alone.
  Write(project / "externals/nested/definitions/b/_card.json",
        "unaccepted broken JSON");
  EXPECT_EQ(service.Handle(input).phase, H::EnvironmentPhase::Ready);
  EXPECT_EQ(helper->launches, 1);
  EXPECT_EQ(helper->signals, 0);
  Write(project / "externals/nested/definitions/b/_card.json",
        HostFolderCard("data", true));
  Write(project / "externals/nested/definitions/b/README.md",
        host::CardReadme("b"));
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
  Write(project / "tsugumi.json",
        FixtureProjectDocument(
            R"({"nixPath": [],"canvas":{"cards":[],"connections":[]}})"));
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
  // The guest started with the new share but the helper's answer was lost:
  // the process found afterwards holds the attempted mounts, not the old ones,
  // so it is stopped and the project starts again with the original set.
  const auto original_mounts = helper->received.mounts;
  auto launches = helper->launches.load();
  auto signals = helper->signals.load();
  helper->lose_reply_once = true;
  EXPECT_THROW(service.Attach(attach, commit), std::runtime_error);
  EXPECT_EQ(helper->launches, launches + 2);
  EXPECT_EQ(helper->signals, signals + 2);
  EXPECT_TRUE(helper->running);
  EXPECT_EQ(helper->received.mounts, original_mounts);
  EXPECT_EQ(helper->received.folders, original);
  const auto restored = service.Handle(start);
  EXPECT_EQ(restored.phase, H::EnvironmentPhase::Ready);
  EXPECT_EQ(restored.folders, original);
  EXPECT_EQ(helper->launches, launches + 2);
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

TEST(EnvironmentAdapterTest,
     StartupRequirementsMustMatchTheAcceptedCoreDocument) {
  TemporaryDirectory directory;
  const auto project = directory.path / "project",
             data = directory.path / "data";
  std::filesystem::create_directory(data);
  Write(
      project / "tsugumi.json",
      FixtureProjectDocument(
          R"({"nixPath": [],"hostPaths":[{"name":"data","description":"Input data"}],"canvas":{"cards":[{"id":"a"}],"connections":[]}})"));
  Write(project / "definitions/a/_card.json", HostFolderCard("data", false));
  Write(project / "definitions/a/README.md", host::CardReadme("a"));
  auto helper = std::make_shared<HelperDouble>();
  helper->guest = true;
  helper->change_requirements = true;
  AuthorizationDouble authorization;
  host::EnvironmentOptions options;
  options.directory = directory.path / "environments";
  options.socket_directory = directory.path / "sockets";
  options.run_scratch_directory = directory.path / "run-scratch";
  options.process = helper;
  options.host_paths = HostPathSettings(project, data);
  host::Environments service(
      options, authorization.Bind(), [](const auto&) {}, [](const auto&) {});
  H::EnvironmentRequestT input;
  input.project = project.string();
  input.action = H::EnvironmentAction::Start;
  const auto state = service.Handle(input);
  EXPECT_EQ(state.phase, H::EnvironmentPhase::Failed);
  EXPECT_FALSE(helper->running);
  EXPECT_FALSE(authorization.active);
}

}  // namespace
}  // namespace tsugumi::test

namespace tsugumi::test {
TEST(EnvironmentAdapterTest, FolderMutationsSurviveStartsAndServiceRecreation) {
  TemporaryDirectory directory;
  const auto project = directory.path / "project";
  Write(project / "tsugumi.json", FixtureProjectDocument("{}"));
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
  const auto has_both_folders = [&](const auto& folders) {
    EXPECT_EQ(std::ranges::count(folders, a), 1);
    EXPECT_EQ(std::ranges::count(folders, b), 1);
    EXPECT_EQ(folders.size(), 2u);
  };
  request.folders = {b};
  has_both_folders(service->Handle(request).folders);
  // An empty/new client starting the environment cannot replace either folder.
  request.action = H::EnvironmentAction::Start;
  request.folders.clear();
  has_both_folders(service->Handle(request).folders);
  has_both_folders(helper->received.folders);
  service.reset();
  service = make();
  request.action = H::EnvironmentAction::State;
  has_both_folders(service->Handle(request).folders);
  request.action = H::EnvironmentAction::RemoveFolders;
  request.folders = {a};
  EXPECT_EQ(service->Handle(request).folders, std::vector<std::string>{b});
  request.folders = {a};
  EXPECT_EQ(service->Handle(request).folders, std::vector<std::string>{b});
  request.action = H::EnvironmentAction::Stop;
  service->Handle(request);
}
TEST(EnvironmentAdapterTest,
     GuestAttachedRootUsesTheMountEvenWhenTheHostPathExists) {
  TemporaryDirectory directory;
  Write(directory.path / "project/tsugumi.json", FixtureProjectDocument());
  auto process = std::make_shared<HelperDouble>();
  process->guest = true;
  host::EnvironmentOptions options;
  options.directory = directory.path / "environments";
  options.socket_directory = directory.path / "sockets";
  options.process = process;
  AuthorizationDouble authorization;
  const auto make = [&] {
    return std::make_unique<host::Environments>(
        options, authorization.Bind(), [](const auto&) {}, [](const auto&) {});
  };
  auto service = make();
  H::EnvironmentRequestT request;
  request.project = (directory.path / "project").string();
  request.action = H::EnvironmentAction::AddFolders;
  request.folders = {"/etc"};
  service->Handle(request);
  request.action = H::EnvironmentAction::Start;
  const auto ready = service->Handle(request);
  ASSERT_EQ(ready.phase, H::EnvironmentPhase::Ready) << ready.message;
  ASSERT_EQ(ready.attached_roots.size(), 1u);
  const auto execution_path = ready.attached_roots[0]->execution_path;
  EXPECT_EQ(ready.attached_roots[0]->host_path, "/etc");
  EXPECT_TRUE(execution_path.starts_with("/run/tsugumi/chat/"));
  EXPECT_NE(execution_path, "/etc");
  service.reset();
  service = make();
  request.action = H::EnvironmentAction::State;
  const auto restored = service->Handle(request);
  ASSERT_EQ(restored.phase, H::EnvironmentPhase::Ready);
  ASSERT_EQ(restored.attached_roots.size(), 1u);
  EXPECT_EQ(restored.attached_roots[0]->execution_path, execution_path);
  request.action = H::EnvironmentAction::Stop;
  service->Handle(request);
}
}  // namespace tsugumi::test
