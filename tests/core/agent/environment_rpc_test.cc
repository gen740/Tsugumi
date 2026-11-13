#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>

#include "fixture.hh"
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

  auto SetUp() -> void override {
    Write(project / "tsugumi.json", R"({"name":"Environment"})");
    extra_agent_arguments = {
        "--tsugumid",        TSUGUMI_ENVIRONMENT_DAEMON,
        "--worker",          TSUGUMI_WORKER_BINARY,
        "--sdk-dir",         TSUGUMI_TEST_SDK,
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
  auto Environment(H::EnvironmentAction action, bool archive = false,
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
               input.archive_incompatible_history = archive;
               input.folders = std::move(folders);
               input.origins = std::move(origins);
               return H::EnvironmentRequest::Pack(b, &input).Union();
             });
    if (!reply.Code().empty()) throw std::runtime_error(reply.Code());
    const auto* state = reply.HostValue()->value_as_EnvironmentState();
    if (!state) throw std::runtime_error("No environment state");
    return *std::unique_ptr<H::EnvironmentStateT>(state->UnPack());
  }
  auto LaunchCount() -> size_t {
    const auto launches = Read(project / "launches");
    return static_cast<size_t>(
        std::count(launches.begin(), launches.end(), '\n'));
  }
  auto Restart() -> void {
    host->OnEvent({});
    host.reset();
    human.reset();
    external.reset();
    agent.Stop();
    StartAgent();
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
    return events_changed.wait_for(lock, 3s, [&] { return HasEvent(type); });
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
    request.server_id = "missing-worker";
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

TEST_F(EnvironmentFixture, StartRegistersIdentityAndAuthorizesBeforeReady) {
  const auto state = Environment(H::EnvironmentAction::Start);
  ASSERT_EQ(state.phase, H::EnvironmentPhase::Ready)
      << state.message << state.log;
  EXPECT_GT(state.pid, 0);
  EXPECT_FALSE(state.endpoint.empty());
  EXPECT_FALSE(state.server_instance_id.empty());
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
  const auto second = Environment(H::EnvironmentAction::Start, false, {}, {},
                                  connection.get(), alias);
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
  EXPECT_NE(next.server_instance_id, first.server_instance_id);
  EXPECT_EQ(LaunchCount(), 2u);
}

TEST_F(EnvironmentFixture,
       LeftoverDaemonIsReauthenticatedAndReusedAfterAgentRestart) {
  const auto first = Environment(H::EnvironmentAction::Start);
  ASSERT_EQ(first.phase, H::EnvironmentPhase::Ready);
  Restart();
  const auto adopted = Environment(H::EnvironmentAction::State);
  EXPECT_EQ(adopted.phase, H::EnvironmentPhase::Ready);
  EXPECT_EQ(adopted.pid, first.pid);
  EXPECT_EQ(Environment(H::EnvironmentAction::Start).server_instance_id,
            first.server_instance_id);
  EXPECT_EQ(LaunchCount(), 1u);
}

TEST_F(EnvironmentFixture,
       RestartObservesResidentDeathWithoutAClientStateQuery) {
  const auto started = Environment(H::EnvironmentAction::Start);
  ASSERT_EQ(started.phase, H::EnvironmentPhase::Ready);
  Restart();
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

TEST_F(EnvironmentFixture, StaleReadinessCannotAdoptOrSignalAnUnrelatedPid) {
  const auto started = Environment(H::EnvironmentAction::Start);
  ASSERT_EQ(started.phase, H::EnvironmentPhase::Ready);
  const auto original = Read(started.ready_file);
  struct Restore {
    std::filesystem::path path;
    std::string contents;
    ~Restore() { Write(path, contents); }
  } restore{started.ready_file, original};
  auto forged = original;
  const auto actual = "\"pid\":" + std::to_string(started.pid);
  const auto offset = forged.find(actual);
  ASSERT_NE(offset, std::string::npos);
  forged.replace(offset, actual.size(),
                 "\"pid\":" + std::to_string(::getpid()));
  Write(started.ready_file, forged);
  Restart();
  EXPECT_EQ(Environment(H::EnvironmentAction::State).phase,
            H::EnvironmentPhase::Stopped);
  EXPECT_EQ(Environment(H::EnvironmentAction::Stop).phase,
            H::EnvironmentPhase::Stopped);
  EXPECT_EQ(::kill(static_cast<pid_t>(started.pid), 0), 0);
}

TEST_F(EnvironmentFixture, IncompatibleHistoryRequiresTheExplicitArchiveFlag) {
  Write(project / "incompatible", "history to preserve");
  const auto refused = Environment(H::EnvironmentAction::Start);
  ASSERT_EQ(refused.phase, H::EnvironmentPhase::Failed);
  EXPECT_FALSE(refused.startup_error_json.empty());
  EXPECT_EQ(Read(project / "incompatible"), "history to preserve");
  EXPECT_FALSE(
      std::filesystem::exists(project / "archived-by-explicit-request"));
  EXPECT_FALSE(
      std::filesystem::exists(std::filesystem::path(refused.control_dir) /
                              "execution-broker-capability"));
  const auto accepted = Environment(H::EnvironmentAction::Start, true);
  EXPECT_EQ(accepted.phase, H::EnvironmentPhase::Ready) << accepted.message;
  EXPECT_EQ(Read(project / "archived-by-explicit-request"),
            "history to preserve");
}

TEST_F(EnvironmentFixture, FoldersCanBeWithdrawnAndOmittedVectorsMeanEmpty) {
  const auto folder = directory.path / "attached";
  std::filesystem::create_directory(folder);
  const auto state =
      Environment(H::EnvironmentAction::Start, false, {folder.string()});
  ASSERT_EQ(state.phase, H::EnvironmentPhase::Ready);
  const auto attached =
      std::filesystem::path(state.control_dir) / "attached.json";
  EXPECT_EQ(JsonString(Read(attached), "/folders/0"), folder.string());
  const auto withdrawn = Environment(H::EnvironmentAction::Folders);
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
}  // namespace
}  // namespace tsugumi::test
