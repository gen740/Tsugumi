#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <future>
#include <set>
#include <thread>

#include "tsugumi/rpc/frame.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/workerd/local.hh"

namespace tsugumi::workerd {
namespace {
struct SocketPair {
  int fd[2]{-1, -1};
  SocketPair() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) != 0)
      throw std::runtime_error("socketpair");
  }
  ~SocketPair() {
    for (int value : fd)
      if (value >= 0) ::close(value);
  }
};

TEST(LocalWorker, HandsOverThreeChannelsWithoutRelayingTheirBytes) {
  SocketPair broker;
  std::array<SocketPair, 3> channels;
  WorkerProc::Descriptors descriptors;
  for (size_t i = 0; i < channels.size(); ++i)
    descriptors[i] = channels[i].fd[0];
  Tsugumi::LocalWorker::MessageT message;
  message.lease = 42;
  message.call_id = 19;
  message.scratch = "/worker/scratch";
  send_local(broker.fd[0], message, &descriptors);
  LocalPacket packet;
  ASSERT_TRUE(receive_local(broker.fd[1], packet));
  EXPECT_EQ(packet.message->lease, 42);
  EXPECT_EQ(packet.message->call_id, 19);
  EXPECT_EQ(packet.message->scratch, "/worker/scratch");
  // Management can disappear; the borrowed channels remain direct.
  ::close(broker.fd[0]);
  broker.fd[0] = -1;
  ::close(broker.fd[1]);
  broker.fd[1] = -1;
  for (size_t i = 0; i < channels.size(); ++i) {
    ASSERT_GE(packet.descriptors[i], 0);
    EXPECT_NE(::fcntl(packet.descriptors[i], F_GETFD) & FD_CLOEXEC, 0);
    const char sent = static_cast<char>('a' + i);
    ASSERT_EQ(::write(packet.descriptors[i], &sent, 1), 1);
    char received = 0;
    ASSERT_EQ(::read(channels[i].fd[1], &received, 1), 1);
    EXPECT_EQ(received, sent);
  }
}

TEST(LocalWorker, EmptyControlMessagesNeedNoDescriptors) {
  SocketPair broker;
  Tsugumi::LocalWorker::MessageT message;
  message.action = Tsugumi::LocalWorker::Action::Clear;
  send_local(broker.fd[0], message);
  LocalPacket packet;
  ASSERT_TRUE(receive_local(broker.fd[1], packet));
  EXPECT_EQ(packet.message->action, Tsugumi::LocalWorker::Action::Clear);
  EXPECT_EQ(packet.descriptors, (WorkerProc::Descriptors{-1, -1, -1}));
}

TEST(LocalWorker, ExceptionalBorrowerExitReturnsTheLeaseAfterClosingChannels) {
  std::array<SocketPair, 3> channels;
  WorkerProc::Descriptors descriptors;
  for (size_t i = 0; i < channels.size(); ++i) {
    descriptors[i] = ::dup(channels[i].fd[0]);
    ASSERT_GE(descriptors[i], 0);
    ::fcntl(descriptors[i], F_SETFL, O_NONBLOCK);
  }
  bool returned = false;
  {
    WorkerProc::Borrowed borrowed{descriptors,
                                  {},
                                  {},
                                  0,
                                  1,
                                  [] {},
                                  [](bool) {},
                                  [&] {
                                    returned = true;
                                    for (int fd : descriptors)
                                      EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
                                  }};
    WorkerProc worker(std::move(borrowed));
    EXPECT_FALSE(returned);
  }
  EXPECT_TRUE(returned);
}

TEST(LocalWorker, DisconnectPreservesTheOutcomeWhenTerminationFails) {
  std::array<SocketPair, 3> channels;
  WorkerProc::Descriptors descriptors;
  for (size_t i = 0; i < channels.size(); ++i) {
    descriptors[i] = ::dup(channels[i].fd[0]);
    ASSERT_GE(descriptors[i], 0);
    ::fcntl(descriptors[i], F_SETFL, O_NONBLOCK);
  }
  bool terminated = false;
  WorkerProc worker({descriptors,
                     {},
                     {},
                     0,
                     1,
                     [] {},
                     [&](bool) {
                       terminated = true;
                       throw std::system_error(ECHILD, std::generic_category(),
                                               "cannot wait for worker");
                     },
                     [] {}});
  auto peer = std::async(std::launch::async, [&] {
    EXPECT_EQ(rpc::frame::Read(channels[0].fd[1], 4096), "call");
    EXPECT_EQ(::shutdown(channels[0].fd[1], SHUT_RDWR), 0);
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  const auto outcome = worker.call("call", 1, {}, deadline);
  peer.get();
  ASSERT_TRUE(outcome.failure);
  EXPECT_EQ(outcome.failure->kind, FailureKind::worker_failed);
  EXPECT_TRUE(terminated);
}

TEST(LocalWorker, StopRequestWakesACallWithoutADeadline) {
  namespace W = Tsugumi::Worker;
  std::array<SocketPair, 3> channels;
  WorkerProc::Descriptors descriptors;
  for (size_t i = 0; i < channels.size(); ++i) {
    descriptors[i] = ::dup(channels[i].fd[0]);
    ASSERT_GE(descriptors[i], 0);
    ::fcntl(descriptors[i], F_SETFL, O_NONBLOCK);
  }
  const timeval timeout{3, 0};
  for (size_t i : {0, 1})
    ASSERT_EQ(::setsockopt(channels[i].fd[1], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                           sizeof timeout),
              0);
  WorkerProc worker({descriptors, {}, {}, 0, 1, [] {}, [](bool) {}, [] {}});
  std::stop_source stop;
  auto peer = std::async(std::launch::async, [&] {
    // Closing the connection also unblocks the call if a check fails.
    struct Disconnect {
      int fd;
      ~Disconnect() { ::shutdown(fd, SHUT_RDWR); }
    } disconnect{channels[0].fd[1]};
    EXPECT_EQ(rpc::frame::Read(channels[0].fd[1], 4096), "call");
    stop.request_stop();
    const auto cancel = rpc::frame::Read(channels[1].fd[1], 4096);
    ASSERT_TRUE(cancel);
    const auto* message = W::GetWorkerMessage(cancel->data());
    EXPECT_EQ(message->call_id(), 1);
    EXPECT_NE(message->value_as_WorkerCancel(), nullptr);

    flatbuffers::FlatBufferBuilder builder;
    const auto result = W::CreateWorkerResult(
        builder, builder.CreateVector(
                     std::vector<flatbuffers::Offset<W::WorkerOutput>>{}));
    W::FinishWorkerMessageBuffer(
        builder,
        W::CreateWorkerMessage(builder, 1, W::WorkerPayload::WorkerResult,
                               result.Union()));
    rpc::frame::Write(
        channels[0].fd[1],
        {reinterpret_cast<const char*>(builder.GetBufferPointer()),
         builder.GetSize()},
        4096);
  });
  const auto outcome = worker.call("call", 1, stop.get_token(), std::nullopt);
  peer.get();
  ASSERT_TRUE(outcome.failure);
  EXPECT_EQ(outcome.failure->kind, FailureKind::cancelled);
}

TEST(LocalWorker, DiscardsStaleRepliesAndRelaysViewsWhileIdle) {
  namespace W = Tsugumi::Worker;
  std::array<SocketPair, 3> channels;
  WorkerProc::Descriptors descriptors;
  for (size_t i = 0; i < channels.size(); ++i) {
    descriptors[i] = ::dup(channels[i].fd[0]);
    ASSERT_GE(descriptors[i], 0);
    ::fcntl(descriptors[i], F_SETFL, O_NONBLOCK);
  }
  std::promise<WorkerDisplayData> received;
  WorkerProc worker({descriptors, {}, {}, 7, 1, [] {}, [](bool) {}, [] {}});
  auto data = received.get_future();
  worker.set_display([&](auto value, auto complete) {
    received.set_value(std::move(value));
    complete();
  });
  const auto send = [&](uint64_t id, W::WorkerPayload kind,
                        flatbuffers::FlatBufferBuilder& builder,
                        flatbuffers::Offset<void> value) {
    W::FinishWorkerMessageBuffer(
        builder, W::CreateWorkerMessage(builder, id, kind, value));
    rpc::frame::Write(
        channels[0].fd[1],
        {reinterpret_cast<const char*>(builder.GetBufferPointer()),
         builder.GetSize()},
        4096);
  };
  auto peer = std::async(std::launch::async, [&] {
    const auto call = rpc::frame::Read(channels[0].fd[1], 4096);
    EXPECT_EQ(call, "call");
    flatbuffers::FlatBufferBuilder b;
    const auto output =
        b.CreateVector(std::vector<flatbuffers::Offset<W::WorkerOutput>>{});
    const auto result = W::CreateWorkerResult(b, output);
    send(7, W::WorkerPayload::WorkerResult, b, result.Union());
    b.Clear();
    const auto current = W::CreateWorkerResult(
        b, b.CreateVector(std::vector<flatbuffers::Offset<W::WorkerOutput>>{}));
    send(8, W::WorkerPayload::WorkerResult, b, current.Union());
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  EXPECT_FALSE(
      worker.call("call", worker.next_call_id(), {}, deadline).failure);
  peer.get();
  worker.idle();
  flatbuffers::FlatBufferBuilder b;
  const uint8_t bytes[] = {0, 255, 128};
  const ViewUuid session{1};
  const W::ViewUuid address{flatbuffers::span<const uint8_t, 16>(session)};
  const auto display =
      W::CreateWorkerDisplay(b, &address, b.CreateVector(bytes, 3));
  send(0, W::WorkerPayload::WorkerDisplay, b, display.Union());
  ASSERT_EQ(data.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  const auto value = data.get();
  EXPECT_EQ(value.session, session);
  EXPECT_EQ(value.bytes, std::string("\0\xff\x80", 3));
  worker.set_display({});

  std::promise<WorkerViewRequest> control;
  auto request = control.get_future();
  worker.set_view_control(
      [&](const auto& incoming) -> std::optional<WorkerViewReply> {
        control.set_value(incoming);
        return std::nullopt;
      });
  b.Clear();
  const auto open = W::CreateWorkerViewRequest(
      b, 1, W::ViewControlKind::Open, b.CreateString("plot"), 1, 640, 480);
  send(0, W::WorkerPayload::WorkerViewRequest, b, open.Union());
  ASSERT_EQ(request.wait_for(std::chrono::seconds(3)),
            std::future_status::ready);
  const auto opening = request.get();
  EXPECT_EQ(opening.request_id, 1);
  EXPECT_EQ(opening.kind, ViewControlKind::open);
  EXPECT_EQ(opening.view, "plot");
  EXPECT_EQ(opening.width, 640);
  const auto receiver = worker.view_receiver().lock();
  ASSERT_TRUE(receiver);
  const ViewUuid route{2};
  EXPECT_FALSE(receiver->reply({2, 0, session, route}));
  EXPECT_TRUE(receiver->reply({1, 0, session, route}));
  EXPECT_FALSE(receiver->reply({1, 0, session, route}));
  EXPECT_EQ(receiver->send({ViewEventKind::close, route, session, {}}),
            WorkerProc::ViewDelivery::accepted);
  EXPECT_EQ(receiver->send({ViewEventKind::close, route, session, {}}),
            WorkerProc::ViewDelivery::closed);

  const timeval timeout{3, 0};
  ASSERT_EQ(::setsockopt(channels[1].fd[1], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout)),
            0);
  std::set<W::WorkerPayload> seen;
  for (int i = 0; i < 3; ++i) {
    const auto frame = rpc::frame::Read(channels[1].fd[1], 4096);
    ASSERT_TRUE(frame);
    const auto* message = W::GetWorkerMessage(frame->data());
    EXPECT_EQ(message->call_id(), 0);
    EXPECT_TRUE(seen.insert(message->value_type()).second);
    if (const auto* reply = message->value_as_WorkerViewReply()) {
      EXPECT_EQ(reply->request_id(), 1);
      EXPECT_EQ(reply->status(), 0);
      ASSERT_TRUE(reply->session());
      EXPECT_EQ(reply->session()->bytes()->Get(0), 1);
    }
    if (const auto* event = message->value_as_WorkerViewEvent()) {
      EXPECT_EQ(event->kind(), W::ViewEventKind::Close);
      EXPECT_EQ(event->route()->bytes()->Get(0), 2);
      EXPECT_EQ(event->session()->bytes()->Get(0), 1);
    }
  }
  EXPECT_EQ(seen, (std::set{W::WorkerPayload::WorkerDisplayCredit,
                            W::WorkerPayload::WorkerViewReply,
                            W::WorkerPayload::WorkerViewEvent}));
}

TEST(LocalWorker, AnInvalidatedLeaseCannotReturnItsResident) {
  const auto scratch = std::filesystem::temp_directory_path() /
                       ("tsugumi-resident-" + crypto::uuid4());
  WorkerPool pool(scratch);
  WorkerSpawn spawn;
  spawn.worker_binary = TSUGUMI_WORKER_BINARY;
  spawn.sandbox.filesystem = false;
  auto first = pool.acquire("card|1", "version", spawn, true);
  EXPECT_THROW(pool.acquire("card|1", "version", spawn, true), Failed);
  pool.invalidate("card", "definition changed");
  pool.release("card|1", first.generation, std::move(first.worker), true);
  auto next = pool.acquire("card|1", "version", spawn, true);
  EXPECT_FALSE(next.reused);
  EXPECT_EQ(next.replaced, "definition changed");
  pool.release("card|1", next.generation, std::move(next.worker), false);
}

TEST(LocalWorker, CoreConnectionCloseEndsAnUnusedPool) {
  SocketPair broker;
  const auto scratch = std::filesystem::temp_directory_path() /
                       ("tsugumi-local-" + crypto::uuid4());
  std::thread daemon([&] { serve_local(std::array{broker.fd[1]}, scratch); });
  Tsugumi::LocalWorker::MessageT request;
  request.action = Tsugumi::LocalWorker::Action::Clear;
  send_local(broker.fd[0], request);
  LocalPacket reply;
  EXPECT_TRUE(receive_local(broker.fd[0], reply));
  EXPECT_TRUE(reply.message->error.empty());
  ::shutdown(broker.fd[0], SHUT_RDWR);
  daemon.join();
  EXPECT_FALSE(std::filesystem::exists(scratch));
}

TEST(LocalWorker, OversizedAcquirePreservesTheConnection) {
  const auto scratch = std::filesystem::temp_directory_path() /
                       ("tsugumi-local-" + crypto::uuid4());
  LocalWorkers workers(TSUGUMI_WORKERD_BINARY, scratch);
  WorkerSpawn spawn;
  spawn.worker_binary = TSUGUMI_WORKER_BINARY;
  spawn.sandbox.filesystem = false;
  auto resident = workers.acquire("card", "version", spawn, true);
  workers.release(std::move(resident.worker), true, {});
  spawn.closure.emplace_back(1 << 20, 'x');
  EXPECT_THROW(workers.acquire("card", "version", spawn, true), Failed);
  spawn.closure.clear();
  auto next = workers.acquire("card", "version", spawn, true);
  EXPECT_TRUE(next.reused);
  workers.release(std::move(next.worker), false, {});
}

TEST(LocalWorker, ABlockedFreshStartDoesNotDelayAnotherAcquire) {
  using namespace std::chrono_literals;
  const auto root = std::filesystem::temp_directory_path() /
                    ("tsugumi-parallel-local-" + crypto::uuid4());
  struct Cleanup {
    const std::filesystem::path& root;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }
  } cleanup{root};
  const auto entered = root / "entered";
  const auto gate = root / "gate";
  const auto launcher = root / "slow-worker";
  const auto script = "#!/bin/sh\n: > '" + entered.string() +
                      "'\nread -r unused < '" + gate.string() +
                      "'\nexec '" TSUGUMI_WORKER_BINARY "' \"$@\"\n";
  fs::write_atomic(launcher, script, 0700);
  ASSERT_EQ(::mkfifo(gate.c_str(), 0600), 0);
  struct Gate {
    int fd;
    ~Gate() {
      if (fd >= 0) ::close(fd);
    }
  } gate_writer{::open(gate.c_str(), O_RDWR | O_CLOEXEC)};
  ASSERT_GE(gate_writer.fd, 0);

  LocalWorkers workers(TSUGUMI_WORKERD_BINARY, root / "scratch");
  WorkerSpawn fast;
  fast.worker_binary = TSUGUMI_WORKER_BINARY;
  fast.sandbox = {false, false, false, false, {}};
  fast.network = true;
  auto slow = fast;
  slow.worker_binary = launcher;
  slow.secrets = std::make_shared<WorkerSecrets>();
  // The launcher's gate holds back reads of the synthetic startup frame.
  slow.secrets->values.emplace_back("fixture", std::string(900 << 10, 'x'));
  auto first = std::async(std::launch::async, [&] {
    return workers.acquire("slow", "slow", slow, false);
  });
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!std::filesystem::exists(entered) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  const bool entered_gate = std::filesystem::exists(entered);
  auto second = std::async(std::launch::async, [&] {
    return workers.acquire("fast", "fast", fast, false);
  });
  const auto fast_status = second.wait_for(3s);
  const auto slow_status = first.wait_for(0ms);
  EXPECT_EQ(::write(gate_writer.fd, "\n", 1), 1);
  EXPECT_TRUE(entered_gate);
  EXPECT_EQ(fast_status, std::future_status::ready);
  EXPECT_EQ(slow_status, std::future_status::timeout);
  auto fast_lease = second.get();
  auto slow_lease = first.get();
  workers.release(std::move(fast_lease.worker), false, {});
  workers.release(std::move(slow_lease.worker), false, {});
}

TEST(LocalWorker, RejectsOversizedAndUnverifiedControlFrames) {
  for (const auto& bytes :
       {std::string("\xff\xff\xff\x7f", 4), std::string("\x01\0\0\0x", 5)}) {
    SocketPair broker;
    ASSERT_EQ(::write(broker.fd[0], bytes.data(), bytes.size()),
              static_cast<ssize_t>(bytes.size()));
    LocalPacket packet;
    EXPECT_THROW(receive_local(broker.fd[1], packet), std::runtime_error);
  }
}
}  // namespace
}  // namespace tsugumi::workerd
