#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

#include "agent/wire.hh"
#include "host/service.hh"
#include "host/wire_endpoint.hh"
#include "tsugumi/client/client.hh"
#include "tsugumi/rpc/lanes.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi::test {
namespace {
using namespace std::chrono_literals;
namespace H = Tsugumi::Host;
namespace S = Tsugumi::Service;

class RuntimeDouble final : public host::EnvironmentProcess {
 public:
  std::atomic<bool> busy = false, running = false, stopped = false;
  std::mutex mutex;
  std::condition_variable changed;
  bool terminal_started = false, release_terminal = false;
  auto Busy(const H::EnvironmentStateT&) -> bool override { return busy; }
  auto Requirements(const H::EnvironmentStateT&)
      -> Tsugumi::RPC::ProjectHostRequirementsT override {
    return {};
  }
  auto Launch(const host::EnvironmentLaunch& launch) -> int64_t override {
    // The ready file must report the identity assigned by hostd, matching the
    // real daemon. A made-up id fails registration before activity is queried.
    const auto id = fs::read_file(std::filesystem::path(launch.project) /
                                  ".tsugumi/project-id");
    if (!id) throw std::runtime_error("Host did not prepare project identity");
    fs::write_atomic(
        std::filesystem::path(launch.control) / "server.ready",
        "{\"protocol\":1,\"pid\":42,\"endpoint\":\"ws://localhost:1/rpc\","
        "\"token\":\"fixture\",\"project_id\":\"" +
            *id +
            "\","
            "\"server_instance_id\":\"fixture\",\"unix_socket\":\"fixture\"}",
        0600);
    running = true;
    return 42;
  }
  auto Poll(int64_t, bool) -> std::optional<int> override {
    return running ? std::nullopt : std::optional<int>(0);
  }
  auto Signal(int64_t, int) -> void override { running = false; }
  auto Verify(const H::EnvironmentStateT&) -> bool override { return running; }
  auto Shutdown() -> void override {
    stopped = true;
    running = false;
    {
      std::lock_guard lock(mutex);
      release_terminal = true;
    }
    changed.notify_all();
  }
  auto Terminal(const H::HostTerminalRequestT&) -> H::HostTerminalT override {
    std::unique_lock lock(mutex);
    terminal_started = true;
    changed.notify_all();
    changed.wait(lock, [&] { return release_terminal; });
    return {};
  }
};

class VMLifetime : public ::testing::Test {
 protected:
  std::shared_ptr<RuntimeDouble> runtime = std::make_shared<RuntimeDouble>();
  std::unique_ptr<host::Service> service;
  std::unique_ptr<TransportServer> server;
  std::filesystem::path directory;
  client::Options options;
  auto SetUp() -> void override {
    char pattern[] = "/tmp/t-life-XXXXXX";
    const auto* made = ::mkdtemp(pattern);
    ASSERT_NE(made, nullptr);
    directory = made;
    std::filesystem::create_directories(directory / "project/.tsugumi");
    fs::write_atomic(directory / "project/tsugumi.json", "{}", 0600);
    host::HostOptions config;
    config.state_dir = directory / "state";
    // Use the production preparation: the store and broker require a private,
    // versioned state directory, not an ordinary mkdir with the process umask.
    std::filesystem::create_directories(config.state_dir);
    ::chmod(config.state_dir.c_str(), 0700);
    config.user_token = "user";
    config.external_token = "external";
    config.server_id = "fixture";
    config.broker_socket = (directory / "broker").string();
    config.environment.directory = directory / "env";
    config.environment.socket_directory = directory / "sockets";
    config.environment.run_scratch_directory = directory / "scratch";
    config.environment.process = runtime;
    service = std::make_unique<host::Service>(config);
    ServerOptions listen;
    HostEndpoint(listen);
    listen.unix_path = (directory / "s").string();
    options.endpoint = "unix:" + listen.unix_path;
    options.protocol = client::Protocol::Host;
    server = std::make_unique<TransportServer>(listen, service.get());
    service->Attach(nullptr, server.get());
    server->start();
  }
  auto TearDown() -> void override {
    if (service) service->Shutdown();
    if (server) server->stop();
    server.reset();
    service.reset();
    std::filesystem::remove_all(directory);
  }
  template <class T>
  auto Call(client::Connection& connection, T value) -> client::Bytes {
    auto result =
        connection
            .Call(
                [value = std::move(value)](uint64_t id) mutable {
                  H::RequestT request;
                  request.value.Set(value);
                  H::MessageT message;
                  message.request_id = id;
                  message.payload.Set(std::move(request));
                  flatbuffers::FlatBufferBuilder b;
                  H::FinishMessageBuffer(b, H::Message::Pack(b, &message));
                  return client::Bytes(b.GetBufferPointer(),
                                       b.GetBufferPointer() + b.GetSize());
                },
                2s)
            .get();
    if (result.error) throw std::runtime_error(result.error.message);
    const auto* reply = H::GetMessage(result.bytes.data())->payload_as_Reply();
    if (const auto* failure = reply->value_as_Failure())
      throw std::runtime_error(failure->message()->str());
    return result.bytes;
  }
  auto Connect() -> std::unique_ptr<client::Connection> {
    auto connection = std::make_unique<client::Connection>(options);
    if (auto error = connection->Start().get())
      throw std::runtime_error(error.message);
    S::HelloRequestT hello;
    hello.token = "user";
    hello.client_id = "vm-lifetime-test";
    Call(*connection, hello);
    return connection;
  }
  auto Busy(client::Connection& connection) -> bool {
    const auto bytes = Call(connection, H::HostActivityRequestT{});
    return H::GetMessage(bytes.data())
        ->payload_as_Reply()
        ->value_as_HostActivity()
        ->busy();
  }
  auto Start(client::Connection& connection) -> void {
    H::EnvironmentRequestT start;
    start.project = (directory / "project").string();
    start.action = H::EnvironmentAction::Start;
    const auto bytes = Call(connection, start);
    ASSERT_EQ(H::GetMessage(bytes.data())
                  ->payload_as_Reply()
                  ->value_as_EnvironmentState()
                  ->phase(),
              H::EnvironmentPhase::Ready);
  }
};

TEST_F(VMLifetime,
       IdleConnectionsDoNotCountAsWorkAndObservationDoesNotStopRuntime) {
  auto app = Connect(), other = Connect();
  EXPECT_FALSE(Busy(*app));
  Start(*other);
  EXPECT_FALSE(Busy(*app));
  EXPECT_TRUE(runtime->running);
  EXPECT_FALSE(runtime->stopped);
  service->Shutdown();
  EXPECT_TRUE(runtime->stopped);
  EXPECT_FALSE(runtime->running);
}
TEST_F(VMLifetime, WorkInAnotherClientsProjectCountsForQuit) {
  auto app = Connect(), other = Connect();
  Start(*other);
  runtime->busy = true;
  EXPECT_TRUE(Busy(*app));
  EXPECT_TRUE(runtime->running);
  runtime->busy = false;
  EXPECT_FALSE(Busy(*app));
}
TEST_F(VMLifetime, RunningHostRequestCountsAfterItsQueueIsEmpty) {
  auto app = Connect(), other = Connect();
  Start(*other);
  auto pending = std::async(std::launch::async, [&] {
    H::HostTerminalRequestT request;
    request.project = (directory / "project").string();
    request.action = H::TerminalAction::Open;
    return Call(*other, request);
  });
  {
    std::unique_lock lock(runtime->mutex);
    ASSERT_TRUE(runtime->changed.wait_for(
        lock, 2s, [&] { return runtime->terminal_started; }));
  }
  EXPECT_TRUE(Busy(*app));
  service->Shutdown();
  EXPECT_TRUE(runtime->stopped);
  EXPECT_NO_THROW(pending.get());
}
TEST(VMLifetimeLane, QueuedAndRunningTasksCountAsWork) {
  Lane lane(1);
  std::mutex mutex;
  std::condition_variable changed;
  bool started = false, release = false;
  ASSERT_TRUE(lane.submit([&]() {
    std::unique_lock lock(mutex);
    started = true;
    changed.notify_all();
    changed.wait(lock, [&] { return release; });
  }));
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, 2s, [&] { return started; }));
  }
  EXPECT_EQ(lane.queued(), 0);
  EXPECT_EQ(lane.pending(), 1);
  EXPECT_FALSE(lane.idle());
  EXPECT_TRUE(lane.submit([]() {}));
  EXPECT_EQ(lane.pending(), 2);
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  changed.notify_all();
  lane.stop();
  EXPECT_TRUE(lane.idle());
  EXPECT_EQ(lane.pending(), 0);
}
}  // namespace
}  // namespace tsugumi::test
