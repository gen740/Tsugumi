#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thread>

#include "tsugumi/util/crypto.hh"
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

TEST(LocalWorker, HandsOverFourChannelsWithoutRelayingTheirBytes) {
  SocketPair broker;
  std::array<SocketPair, 4> channels;
  std::array<int, 4> descriptors;
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
  EXPECT_EQ(packet.descriptors, (std::array<int, 4>{-1, -1, -1, -1}));
}

TEST(LocalWorker, ExceptionalBorrowerExitReturnsTheLeaseAfterClosingChannels) {
  std::array<SocketPair, 4> channels;
  std::array<int, 4> descriptors;
  for (size_t i = 0; i < channels.size(); ++i) {
    descriptors[i] = ::dup(channels[i].fd[0]);
    ASSERT_GE(descriptors[i], 0);
    ::fcntl(descriptors[i], F_SETFL, O_NONBLOCK);
  }
  bool returned = false;
  {
    WorkerProc::Borrowed borrowed{descriptors,
                                  {},
                                  0,
                                  1,
                                  false,
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

TEST(LocalWorker, CoreConnectionCloseEndsAnUnusedPool) {
  SocketPair broker;
  const auto scratch = std::filesystem::temp_directory_path() /
                       ("tsugumi-local-" + crypto::uuid4());
  std::thread daemon([&] { serve_local(broker.fd[1], scratch); });
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
