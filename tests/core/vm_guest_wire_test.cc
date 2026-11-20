#include <gtest/gtest.h>

#include <array>
#include <thread>

#include "../../vm/guest/src/control_wire.hh"

namespace tsugumi::test {
namespace {

namespace guest = vm_guest;
namespace G = guest::wire;

class GuestWire : public ::testing::Test {
 protected:
  int sockets[2]{-1, -1};
  auto SetUp() -> void override {
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  }
  auto TearDown() -> void override {
    for (const auto fd : sockets)
      if (fd >= 0) ::close(fd);
  }
};

TEST_F(GuestWire, FragmentedRequestAndFollowingFrameAreReadSeparately) {
  G::StartT launch;
  launch.key = "a012";
  launch.host_name = "project";
  launch.origins = {"https://example.test", "http://localhost:8000"};
  G::RequestT request;
  request.command.Set(std::move(launch));
  G::MessageT message;
  message.request_id = 22;
  message.payload.Set(std::move(request));
  flatbuffers::FlatBufferBuilder builder;
  G::FinishMessageBuffer(builder, G::Message::Pack(builder, &message));
  std::array<uint8_t, sizeof(uint32_t)> prefix;
  flatbuffers::WriteScalar<uint32_t>(prefix.data(), builder.GetSize());
  ASSERT_TRUE(guest::WriteAll(sockets[0], prefix.data(), 2));
  std::jthread writer([&] {
    EXPECT_TRUE(guest::WriteAll(sockets[0], prefix.data() + 2, 2));
    EXPECT_TRUE(guest::WriteAll(sockets[0], builder.GetBufferPointer(),
                                builder.GetSize()));
    EXPECT_TRUE(guest::Send(sockets[0], G::ReadyT{}));
    ::shutdown(sockets[0], SHUT_WR);
  });

  const auto first = guest::ReadMessage(sockets[1]);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->request_id, 22);
  const auto* decoded = first->payload.AsRequest();
  ASSERT_NE(decoded, nullptr);
  ASSERT_NE(decoded->command.AsStart(), nullptr);
  EXPECT_EQ(decoded->command.AsStart()->origins,
            (std::vector<std::string>{"https://example.test",
                                      "http://localhost:8000"}));
  const auto second = guest::ReadMessage(sockets[1]);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->request_id, 0);
  EXPECT_NE(second->payload.AsReady(), nullptr);
  EXPECT_EQ(guest::ReadMessage(sockets[1]), nullptr);
}

TEST_F(GuestWire, RejectsTruncatedFrame) {
  const std::array<uint8_t, 6> truncated{4, 0, 0, 0, 1, 1};
  ASSERT_TRUE(guest::WriteAll(sockets[0], truncated.data(), truncated.size()));
  ASSERT_EQ(::shutdown(sockets[0], SHUT_WR), 0);
  EXPECT_THROW(guest::ReadMessage(sockets[1]), std::runtime_error);
}

TEST_F(GuestWire, RejectsInvalidMessagesAndOversizedFrames) {
  const std::array<uint8_t, 8> malformed{4, 0, 0, 0, 0, 0, 0, 0};
  ASSERT_TRUE(guest::WriteAll(sockets[0], malformed.data(), malformed.size()));
  EXPECT_THROW(guest::ReadMessage(sockets[1]), std::runtime_error);
  std::array<uint8_t, sizeof(uint32_t)> prefix;
  flatbuffers::WriteScalar<uint32_t>(prefix.data(), guest::kMaxFrameSize + 1);
  ASSERT_TRUE(guest::WriteAll(sockets[0], prefix.data(), prefix.size()));
  EXPECT_THROW(guest::ReadMessage(sockets[1]), std::runtime_error);
}

}  // namespace
}  // namespace tsugumi::test
