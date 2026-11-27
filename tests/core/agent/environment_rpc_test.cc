#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>

#include "fixture.hh"
#include "tsugumi/document/document.hh"
#include "tsugumi/workerd/protocol.hh"

namespace tsugumi::test {
namespace {
namespace W = Tsugumi::RemoteWorker;
namespace framing = tsugumi::workerd;

class EnvironmentFixture : public AgentFixture {
 protected:
  std::filesystem::path project = directory.path / "project";
  std::mutex events_mutex;
  std::condition_variable events_changed;
  std::vector<client::Bytes> events;

  virtual auto DaemonBinary() -> std::string {
    return TSUGUMI_ENVIRONMENT_DAEMON;
  }
  auto SetUp() -> void override {
    Write(project / "tsugumi.json", initial_project_json("Environment", {}));
    extra_host_arguments = {
        "--tsugumid",        DaemonBinary(),
        "--worker",          TSUGUMI_WORKER_BINARY,
        "--nix-dir",         TSUGUMI_TEST_NIX,
        "--environment-dir", (directory.path / "environments").string()};
    AgentFixture::SetUp();
    Observe();
  }
  auto TearDown() -> void override {
    // No resident fake daemon is left behind by a failed assertion.
    if (host) {
      try {
        Environment(H::EnvironmentAction::Stop);
      } catch (...) {
      }
      host->OnEvent({});
      host.reset();
    }
    AgentFixture::TearDown();
  }
  auto Observe() -> void {
    host->OnEvent([this](client::Bytes bytes) {
      std::lock_guard lock(events_mutex);
      events.push_back(std::move(bytes));
      events_changed.notify_all();
    });
  }
  auto Environment(H::EnvironmentAction action,
                   std::vector<std::string> folders = {},
                   std::vector<std::string> origins = {"http://localhost:8780"},
                   client::Connection* connection = nullptr,
                   std::filesystem::path location = {})
      -> H::EnvironmentStateT {
    const auto reply =
        Call(connection ? *connection : *host,
             H::RequestPayload::EnvironmentRequest, [&](Builder& b) {
               H::EnvironmentRequestT input;
               input.project = (location.empty() ? project : location).string();
               input.action = action;
               input.folders = std::move(folders);
               input.origins = std::move(origins);
               return H::EnvironmentRequest::Pack(b, &input).Union();
             });
    if (!reply.Code().empty()) throw std::runtime_error(reply.Code());
    const auto* state = reply.HostValue()->value_as_EnvironmentState();
    if (!state) throw std::runtime_error("No environment state");
    return *std::unique_ptr<H::EnvironmentStateT>(state->UnPack());
  }
  auto ListEnvironments() -> H::EnvironmentListT {
    const auto reply = Call(*host, H::RequestPayload::EnvironmentListRequest,
                            [](Builder& b) {
                              return H::CreateEnvironmentListRequest(b).Union();
                            });
    const auto* list = reply.HostValue()->value_as_EnvironmentList();
    if (!list) throw std::runtime_error("No environment list");
    return *std::unique_ptr<H::EnvironmentListT>(list->UnPack());
  }
  auto LaunchCount() -> size_t {
    const auto launches = Read(project / "launches");
    return static_cast<size_t>(
        std::count(launches.begin(), launches.end(), '\n'));
  }
  auto Restart(bool crash = false) -> void {
    host->OnEvent({});
    host.reset();
    human.reset();
    external.reset();
    service.Stop(crash);
    StartHost();
    Observe();
  }
  auto HasEvent(H::EventPayload type) -> bool {
    return std::any_of(events.begin(), events.end(), [&](const auto& bytes) {
      return H::GetMessage(bytes.data())->payload_as_Event()->value_type() ==
             type;
    });
  }
  auto WaitForEvent(H::EventPayload type) -> bool {
    std::unique_lock lock(events_mutex);
    events_changed.wait(lock, [&] { return HasEvent(type); });
    return true;
  }
  auto BrokerCode(const std::string& capability) -> W::ErrorCode {
    const auto socket_path = JsonString(ready, "/execution_broker").substr(5);
    const auto fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket");
    struct Close {
      int fd;
      ~Close() { ::close(fd); }
    } close{fd};
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path))
      throw std::runtime_error("socket path");
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
      throw std::runtime_error("connect");
    timeval timeout{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    auto transfer = [&](std::string& bytes, bool send) {
      size_t offset = 0;
      while (offset < bytes.size()) {
        const auto size =
            send ? ::write(fd, bytes.data() + offset, bytes.size() - offset)
                 : ::read(fd, bytes.data() + offset, bytes.size() - offset);
        if (size <= 0) throw std::runtime_error("broker transfer");
        offset += static_cast<size_t>(size);
      }
    };
    W::RequestT request;
    request.service = "worker";
    request.body.Set(W::ProbeRequestT{});
    W::BrokerRequestT broker;
    broker.capability = capability;
    broker.request = std::make_unique<W::RequestT>(std::move(request));
    W::MessageT message;
    message.payload.Set(std::move(broker));
    auto bytes = framing::encode(std::move(message));
    auto prefix = framing::length_prefix(static_cast<uint32_t>(bytes.size()));
    transfer(prefix, true);
    transfer(bytes, true);
    ::shutdown(fd, SHUT_WR);
    transfer(prefix, false);
    bytes.assign(framing::frame_length(prefix), '\0');
    transfer(bytes, false);
    const auto response = framing::decode(bytes);
    const auto* failure = response->payload.AsReply()->body.AsFailure();
    if (!failure)
      throw std::runtime_error("Expected broker failure for missing worker");
    return failure->code;
  }
};

TEST_F(EnvironmentFixture, ListsTheEnvironmentsOwnedByHost) {
  EXPECT_TRUE(ListEnvironments().environments.empty());
  const auto ready = Environment(H::EnvironmentAction::Start);
  ASSERT_EQ(ready.phase, H::EnvironmentPhase::Ready);
  auto listed = ListEnvironments();
  ASSERT_EQ(listed.environments.size(), 1u);
  EXPECT_EQ(listed.environments.front()->name, "Environment");
  EXPECT_EQ(listed.environments.front()->state->key, ready.key);
  EXPECT_EQ(listed.environments.front()->state->project_id, ready.project_id);
  Environment(H::EnvironmentAction::Stop);
  EXPECT_TRUE(ListEnvironments().environments.empty());
}

// This uses the production service and hello validation. Only Nix evaluation
// is replaced, so the daemon double cannot hide a readiness protocol mismatch.
class RealEnvironmentFixture : public EnvironmentFixture {
 protected:
  auto DaemonBinary() -> std::string override { return TSUGUMI_PROJECT_BINARY; }
};
TEST_F(RealEnvironmentFixture, AuthenticatesTheRealProjectServiceBeforeReady) {
  Write(project / "tsugumi.json", initial_project_json("Environment", {}));
  struct Startup {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<H::EnvironmentStateT> states;
    std::string stage;
    std::optional<client::Result> reply;
  };
  const auto startup = std::make_shared<Startup>();
  host->OnEvent([startup](const client::Bytes& bytes) {
    const auto* event = H::GetMessage(bytes.data())->payload_as_Event();
    std::lock_guard lock(startup->mutex);
    if (const auto* state = event->value_as_EnvironmentState())
      startup->states.push_back(
          *std::unique_ptr<H::EnvironmentStateT>(state->UnPack()));
    if (const auto* progress = event->value_as_EnvironmentProgress())
      startup->stage = progress->stage()->str();
    startup->changed.notify_all();
  });
  // Start owns a 60s startup budget (plus bounded stop/kill cleanup). The
  // ordinary fixture Call's 5s RPC deadline is unrelated to authentication.
  // Follow the state stream and keep the RPC budget above the startup budget.
  host->Call(
      [&](uint64_t id) {
        Builder b;
        const auto value = H::CreateEnvironmentRequest(
            b, b.CreateString(project.string()), H::EnvironmentAction::Start);
        H::FinishMessageBuffer(
            b, H::CreateMessage(
                   b, id, H::Payload::Request,
                   H::CreateRequest(b, H::RequestPayload::EnvironmentRequest,
                                    value.Union())
                       .Union()));
        return client::Bytes(b.GetBufferPointer(),
                             b.GetBufferPointer() + b.GetSize());
      },
      [startup](client::Result reply) {
        std::lock_guard lock(startup->mutex);
        startup->reply = std::move(reply);
        startup->changed.notify_all();
      },
      90s);
  std::unique_lock lock(startup->mutex);
  const auto terminal = [&] {
    return !startup->states.empty() &&
           (startup->states.back().phase == H::EnvironmentPhase::Ready ||
            startup->states.back().phase == H::EnvironmentPhase::Failed);
  };
  startup->changed.wait(lock, [&] { return terminal() || startup->reply.has_value(); });
  if (startup->reply)
    ASSERT_FALSE(startup->reply->error) << startup->reply->error.message;
  ASSERT_FALSE(startup->states.empty())
      << "Start returned without state events";
  const auto state = startup->states.back();
  ASSERT_EQ(startup->states.front().phase, H::EnvironmentPhase::Starting);
  ASSERT_EQ(state.phase, H::EnvironmentPhase::Ready)
      << startup->stage << ": " << state.message << "\n"
      << state.log;
  startup->changed.wait(lock, [&] { return startup->reply.has_value(); });
  ASSERT_FALSE(startup->reply->error) << startup->reply->error.message;
  const Reply reply{std::move(startup->reply->bytes)};
  ASSERT_TRUE(reply.Code().empty()) << reply.Code();
  const auto* returned = reply.HostValue()->value_as_EnvironmentState();
  ASSERT_NE(returned, nullptr);
  EXPECT_EQ(returned->phase(), H::EnvironmentPhase::Ready);
  EXPECT_EQ(returned->project_id()->str(), state.project_id);
  EXPECT_EQ(returned->token()->str(), state.token);
  lock.unlock();
  EXPECT_GT(state.pid, 0);
  EXPECT_FALSE(state.project_id.empty());
  EXPECT_FALSE(state.token.empty());
  EXPECT_EQ(Environment(H::EnvironmentAction::Stop).phase,
            H::EnvironmentPhase::Stopped);
}

TEST_F(EnvironmentFixture, StartRegistersIdentityAndAuthorizesBeforeReady) {
  const auto state = Environment(H::EnvironmentAction::Start);
  ASSERT_EQ(state.phase, H::EnvironmentPhase::Ready)
      << state.message << state.log;
  EXPECT_GT(state.pid, 0);
  EXPECT_FALSE(state.endpoint.empty());
  EXPECT_TRUE(WaitForEvent(H::EventPayload::EnvironmentProgress));
  EXPECT_TRUE(WaitForEvent(H::EventPayload::EnvironmentState));
  const auto conversation = Call(
      *human, A::RequestPayload::ConversationCreateRequest, [&](Builder& b) {
        return A::CreateConversationCreateRequest(
                   b, 0, b.CreateString(state.project_id))
            .Union();
      });
  EXPECT_TRUE(conversation.Code().empty());
  const auto record = Read(project / "launches");
  EXPECT_EQ(JsonString(record, "/origins/0"), "http://localhost:8780");
  EXPECT_EQ(JsonString(record, "/broker"),
            JsonString(ready, "/execution_broker"));
  const auto capability_file = JsonString(record, "/capability");
  const auto capability = Read(capability_file);
  ASSERT_FALSE(capability.empty());
  EXPECT_NE(BrokerCode(capability), W::ErrorCode::Unauthorized);
  EXPECT_EQ(Environment(H::EnvironmentAction::Stop).phase,
            H::EnvironmentPhase::Stopped);
  EXPECT_TRUE(WaitForEvent(H::EventPayload::EnvironmentStopped));
  EXPECT_EQ(BrokerCode(capability), W::ErrorCode::Unauthorized);
  EXPECT_FALSE(std::filesystem::exists(capability_file));
  const auto denied = Call(*human, A::RequestPayload::ConversationCreateRequest,
                           [&](Builder& b) {
                             return A::CreateConversationCreateRequest(
                                        b, 0, b.CreateString(state.project_id))
                                 .Union();
                           });
  EXPECT_EQ(denied.Code(), "not_found");
}

TEST_F(EnvironmentFixture, ConcurrentStartsAndSymlinkAliasesShareOneProcess) {
  const auto alias = directory.path / "alias";
  std::filesystem::create_directory_symlink(project, alias);
  auto connection = ConnectHost();
  auto first = std::async(std::launch::async, [&] {
    return Environment(H::EnvironmentAction::Start);
  });
  const auto second =
      Environment(H::EnvironmentAction::Start, {}, {}, connection.get(), alias);
  const auto started = first.get();
  ASSERT_EQ(started.phase, H::EnvironmentPhase::Ready) << started.message;
  EXPECT_EQ(second.pid, started.pid);
  EXPECT_EQ(second.key, started.key);
  EXPECT_EQ(LaunchCount(), 1u);
}

TEST_F(EnvironmentFixture, DeathIsReportedAndAnotherStartCreatesOneNewProcess) {
  const auto first = Environment(H::EnvironmentAction::Start);
  ASSERT_EQ(first.phase, H::EnvironmentPhase::Ready);
  const auto capability = Read(std::filesystem::path(first.control_dir) /
                               "execution-broker-capability");
  ASSERT_EQ(::kill(static_cast<pid_t>(first.pid), SIGKILL), 0);
  EXPECT_TRUE(WaitForEvent(H::EventPayload::EnvironmentStopped));
  EXPECT_EQ(Environment(H::EnvironmentAction::State).phase,
            H::EnvironmentPhase::Failed);
  EXPECT_EQ(BrokerCode(capability), W::ErrorCode::Unauthorized);
  const auto next = Environment(H::EnvironmentAction::Start);
  ASSERT_EQ(next.phase, H::EnvironmentPhase::Ready) << next.message;
  EXPECT_NE(next.token, first.token);
  EXPECT_EQ(LaunchCount(), 2u);
}

TEST_F(EnvironmentFixture,
       LeftoverDaemonIsReauthenticatedAndReusedAfterHostRestart) {
  const auto first = Environment(H::EnvironmentAction::Start);
  ASSERT_EQ(first.phase, H::EnvironmentPhase::Ready);
  Restart(true);
  const auto adopted = Environment(H::EnvironmentAction::State);
  EXPECT_EQ(adopted.phase, H::EnvironmentPhase::Ready);
  EXPECT_EQ(adopted.pid, first.pid);
  EXPECT_EQ(Environment(H::EnvironmentAction::Start).pid, first.pid);
  EXPECT_EQ(LaunchCount(), 1u);
}

TEST_F(EnvironmentFixture,
       RestartObservesResidentDeathWithoutAClientStateQuery) {
  const auto started = Environment(H::EnvironmentAction::Start);
  ASSERT_EQ(started.phase, H::EnvironmentPhase::Ready);
  Restart(true);
  ASSERT_EQ(::kill(static_cast<pid_t>(started.pid), SIGKILL), 0);
  EXPECT_TRUE(WaitForEvent(H::EventPayload::EnvironmentStopped));
  EXPECT_EQ(Environment(H::EnvironmentAction::State).phase,
            H::EnvironmentPhase::Failed);
}

TEST_F(EnvironmentFixture, GracefulDaemonExitIsStopped) {
  const auto started = Environment(H::EnvironmentAction::Start);
  ASSERT_EQ(started.phase, H::EnvironmentPhase::Ready);
  ASSERT_EQ(::kill(static_cast<pid_t>(started.pid), SIGTERM), 0);
  EXPECT_TRUE(WaitForEvent(H::EventPayload::EnvironmentStopped));
  EXPECT_EQ(Environment(H::EnvironmentAction::State).phase,
            H::EnvironmentPhase::Stopped);
}

TEST_F(EnvironmentFixture, FoldersCanBeWithdrawnAndOmittedVectorsMeanEmpty) {
  const auto folder = directory.path / "attached";
  std::filesystem::create_directory(folder);
  Environment(H::EnvironmentAction::AddFolders, {folder.string()});
  const auto state = Environment(H::EnvironmentAction::Start);
  ASSERT_EQ(state.phase, H::EnvironmentPhase::Ready);
  const auto attached =
      std::filesystem::path(state.control_dir) / "attached.json";
  EXPECT_EQ(JsonString(Read(attached), "/folders/0"), folder.string());
  const auto withdrawn =
      Environment(H::EnvironmentAction::RemoveFolders, {folder.string()});
  EXPECT_EQ(withdrawn.pid, state.pid);
  EXPECT_EQ(Read(attached), R"({"folders":[]})");
  EXPECT_EQ(LaunchCount(), 1u);
  const auto runtime =
      Call(*host, H::RequestPayload::HostRuntimeRequest,
           [](Builder& b) { return H::CreateHostRuntimeRequest(b).Union(); });
  ASSERT_NE(runtime.HostValue()->value_as_HostRuntime(), nullptr);
  EXPECT_EQ(runtime.HostValue()->value_as_HostRuntime()->vm_state()->str(),
            "unavailable");
}
TEST_F(EnvironmentFixture, AttachingAFolderGrantsTheAgentReadAccessToIt) {
  const auto folder = directory.path / "reference";
  std::filesystem::create_directory(folder);
  const auto reply =
      Call(*host, H::RequestPayload::AttachFoldersRequest, [&](Builder& b) {
        H::AttachFoldersRequestT input;
        input.project = project.string();
        input.folders = {folder.string()};
        return H::AttachFoldersRequest::Pack(b, &input).Union();
      });
  ASSERT_TRUE(reply.Code().empty()) << reply.Code();
  const auto* attached = reply.HostValue()->value_as_AttachedFolders();
  ASSERT_NE(attached, nullptr);
  ASSERT_EQ(attached->folders()->size(), 1u);
  const auto resource = attached->folders()->Get(0)->resource()->str();
  const auto permissions = Call(
      *host, H::RequestPayload::ProjectPermissionsRequest, [](Builder& b) {
        return H::CreateProjectPermissionsRequest(b).Union();
      });
  const auto* list = permissions.HostValue()->value_as_ProjectPermissionList();
  ASSERT_NE(list, nullptr);
  bool found = false;
  for (const auto* entry : *list->projects())
    for (const auto* permission : *entry->permissions())
      if (permission->resource_id()->str() == resource) {
        found = true;
        EXPECT_EQ(permission->access(), Tsugumi::Service::Access::Read);
        EXPECT_TRUE(permission->allowed());
      }
  EXPECT_TRUE(found);
}
}  // namespace
}  // namespace tsugumi::test
