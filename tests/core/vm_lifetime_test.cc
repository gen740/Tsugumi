#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

#include "lanes.hh"
#include "server.hh"
#include "tsugumi/client/client.hh"
#include "tsugumi/protocol/agent_generated.h"

namespace tsugumi::test {
namespace {
using namespace std::chrono_literals;
namespace A = Tsugumi::Agent;
namespace S = Tsugumi::Service;

auto Reply(uint64_t id, bool accepted) -> std::string {
  flatbuffers::FlatBufferBuilder b;
  // Distinguish accepted/busy using the HelloResult's server_id for this
  // transport double; production uses IdleShutdownResult.stopping.
  auto value = S::CreateHelloResult(b, 1, b.CreateString(accepted ? "yes" : "no"), b.CreateString("user"));
  auto reply = A::CreateReply(b, A::ReplyPayload::HelloResult, value.Union());
  A::FinishMessageBuffer(b, A::CreateMessage(b, id, A::Payload::Reply, reply.Union()));
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}
class IdleSink final : public RequestSink {
 public:
  Server* server = nullptr;
  std::atomic<bool> busy = false;
  auto idle() -> bool override { return !busy; }
  auto hello(const RequestIn& request) -> std::pair<std::string, bool> override {
    return {Reply(request.request_id, true), true};
  }
  auto dispatch(RequestIn request) -> void override {
    server->stop_if_idle(request.conn, Reply(request.request_id, true), Reply(request.request_id, false));
  }
  auto disconnected(ConnId) -> void override {}
};
auto Call(client::Connection& connection, bool hello) -> bool {
  auto result = connection.Call([hello](uint64_t id) {
    flatbuffers::FlatBufferBuilder b;
    flatbuffers::Offset<A::Request> request;
    if (hello) {
      auto value = S::CreateHelloRequest(b, b.CreateString("token"), b.CreateString("vm-lifetime-test"), 1);
      request = A::CreateRequest(b, A::RequestPayload::HelloRequest, value.Union());
    } else {
      auto value = A::CreateAccessListRequest(b);
      request = A::CreateRequest(b, A::RequestPayload::AccessListRequest, value.Union());
    }
    A::FinishMessageBuffer(b, A::CreateMessage(b, id, A::Payload::Request, request.Union()));
    return client::Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
  }, 2s).get();
  if (result.error) throw std::runtime_error(result.error.message);
  return A::GetMessage(result.bytes.data())->payload_as_Reply()->value_as_HelloResult()->server_id()->str() == "yes";
}
class VMLifetime : public ::testing::Test {
 protected:
  IdleSink sink;
  std::unique_ptr<Server> server;
  std::string directory;
  client::Options options;
  void SetUp() override {
    char pattern[] = "/tmp/tsugumi-idle-XXXXXX";
    directory = ::mkdtemp(pattern);
    ServerOptions listen;
    listen.protocol = ServerOptions::Protocol::Agent;
    listen.unix_path = directory + "/s";
    options.endpoint = "unix:" + listen.unix_path;
    options.protocol = client::Protocol::Agent;
    server = std::make_unique<Server>(listen, &sink);
    sink.server = server.get();
    server->start();
  }
  void TearDown() override { server->stop(); std::filesystem::remove_all(directory); }
};
TEST_F(VMLifetime, BusyWorkRejectsIdleStopAndReplyIsFlushedBeforeExit) {
  client::Connection connection(options);
  ASSERT_FALSE(connection.Start().get());
  ASSERT_TRUE(Call(connection, true));
  sink.busy = true;
  EXPECT_FALSE(Call(connection, false));
  EXPECT_FALSE(server->idle_expired());
  sink.busy = false;
  EXPECT_TRUE(Call(connection, false));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!server->idle_expired() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  EXPECT_TRUE(server->idle_expired());
}
TEST_F(VMLifetime, AnotherClientPreventsIdleStop) {
  client::Connection first(options), second(options);
  ASSERT_FALSE(first.Start().get());
  ASSERT_TRUE(Call(first, true));
  ASSERT_FALSE(second.Start().get());
  ASSERT_TRUE(Call(second, true));
  EXPECT_FALSE(Call(first, false));
  EXPECT_FALSE(server->idle_expired());
}
TEST(VMLifetimeLane, RunningTaskCountsAsBusyAfterQueueIsEmpty) {
  Lane lane(1, 4, 5s);
  std::mutex mutex;
  std::condition_variable changed;
  bool started = false, release = false;
  ASSERT_TRUE(lane.submit([&](bool) {
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
  EXPECT_FALSE(lane.idle());
  { std::lock_guard lock(mutex); release = true; }
  changed.notify_all();
  lane.stop();
  EXPECT_TRUE(lane.idle());
}
}  // namespace
}  // namespace tsugumi::test
