#include <gtest/gtest.h>
#include <sys/un.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#include "../../vm/guest/src/build_cpu.hh"
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

// tsugumi-vmd's build CPU socket (build_cpu.hh): the answer is about the
// process that connected, never one a request names.
class BuildCpuSocket : public ::testing::Test {
 protected:
  std::filesystem::path folder;
  std::string endpoint;
  int listener = -1;
  int stop[2]{-1, -1};
  std::mutex mutex;
  std::vector<guest::BuildCpuPeer> asked;
  std::jthread server;

  auto SetUp() -> void override {
    const char* base = std::getenv("TSUGUMI_SOCKET_DIR");
    std::string pattern = std::string(base && *base ? base : "/tmp") +
                          "/tsugumi-vmd-XXXXXX";
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    folder = pattern;
    endpoint = (folder / "build-cpu").string();
    listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    const auto address = address_of(endpoint);
    ASSERT_EQ(::bind(listener, reinterpret_cast<const sockaddr*>(&address),
                     sizeof address),
              0);
    ASSERT_EQ(::listen(listener, 64), 0);
    ASSERT_EQ(::pipe(stop), 0);
    server = std::jthread([this] {
      guest::BuildCpuServer(listener, [this](const guest::BuildCpuPeer& peer) {
        std::lock_guard lock(mutex);
        asked.push_back(peer);
        guest::project_wire::BuildCpuReplyT reply;
        reply.cpu_ms = 1000 * asked.size();
        return reply;
      }).Run(stop[0]);
    });
  }
  auto TearDown() -> void override {
    if (stop[1] >= 0) EXPECT_EQ(::write(stop[1], "x", 1), 1);
    if (server.joinable()) server.join();
    for (const auto fd : {listener, stop[0], stop[1]})
      if (fd >= 0) ::close(fd);
    std::filesystem::remove_all(folder);
  }

  static auto address_of(const std::string& path) -> sockaddr_un {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    EXPECT_LT(path.size(), sizeof address.sun_path);
    std::strncpy(address.sun_path, path.c_str(), sizeof address.sun_path - 1);
    return address;
  }
  auto connect() -> int {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    const auto address = address_of(endpoint);
    EXPECT_EQ(::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                        sizeof address),
              0);
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    return fd;
  }
  static auto request() -> std::string {
    flatbuffers::FlatBufferBuilder builder;
    builder.FinishSizePrefixed(
        guest::project_wire::CreateBuildCpuRequest(builder));
    return {reinterpret_cast<const char*>(builder.GetBufferPointer()),
            builder.GetSize()};
  }
  // The reply, or nullptr when the server closed the connection.
  static auto ask(int fd, const std::string& bytes = request())
      -> std::unique_ptr<guest::project_wire::BuildCpuReplyT> {
    try {
      if (::send(fd, bytes.data(), bytes.size(), kNoSignal) !=
          static_cast<ssize_t>(bytes.size()))
        return nullptr;
      std::vector<uint8_t> frame(sizeof(uint32_t));
      if (!guest::ReadExact(fd, frame)) return nullptr;
      frame.resize(sizeof(uint32_t) +
                   flatbuffers::ReadScalar<uint32_t>(frame.data()));
      if (!guest::ReadExact(
              fd, std::span(frame).subspan(sizeof(uint32_t))))
        return nullptr;
      flatbuffers::Verifier verifier(frame.data(), frame.size());
      EXPECT_TRUE(verifier.VerifySizePrefixedBuffer<
                  guest::project_wire::BuildCpuReply>(nullptr));
      return std::unique_ptr<guest::project_wire::BuildCpuReplyT>(
          flatbuffers::GetSizePrefixedRoot<guest::project_wire::BuildCpuReply>(
              frame.data())
              ->UnPack());
    } catch (const std::runtime_error&) {
      return nullptr;  // reset by the server
    }
  }
  static auto closed(int fd) -> bool {
    char byte;
    return ::read(fd, &byte, 1) <= 0;
  }
#ifdef MSG_NOSIGNAL
  static constexpr int kNoSignal = MSG_NOSIGNAL;
#else
  static constexpr int kNoSignal = 0;
#endif
};

TEST_F(BuildCpuSocket, AnswersAboutTheConnectingProcessOnEveryRequest) {
  const int fd = connect();
  const auto first = ask(fd);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->cpu_ms, 1000u);
  EXPECT_TRUE(first->error.empty());
  const auto second = ask(fd);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->cpu_ms, 2000u);
  ::close(fd);
  std::lock_guard lock(mutex);
  ASSERT_EQ(asked.size(), 2u);
  for (const auto& peer : asked) {
    EXPECT_EQ(peer.pid, ::getpid());
    EXPECT_EQ(peer.uid, ::getuid());
  }
}

TEST_F(BuildCpuSocket, AFragmentedOrInvalidRequestHoldsUpNoOtherClient) {
  const int partial = connect();
  const auto bytes = request();
  ASSERT_TRUE(guest::WriteAll(partial, bytes.data(), 2));
  const int other = connect();
  EXPECT_NE(ask(other), nullptr);
  // The rest of the fragmented request is answered.
  const auto reply = ask(partial, bytes.substr(2));
  ASSERT_NE(reply, nullptr);
  EXPECT_TRUE(reply->cpu_ms.has_value());
  // A frame that is no request, or too large to be one, ends the connection.
  const std::array<uint8_t, 8> malformed{4, 0, 0, 0, 0, 0, 0, 0};
  ASSERT_TRUE(guest::WriteAll(other, malformed.data(), malformed.size()));
  EXPECT_TRUE(closed(other));
  std::array<uint8_t, sizeof(uint32_t)> oversized;
  flatbuffers::WriteScalar<uint32_t>(oversized.data(),
                                     guest::kMaxBuildCpuFrame + 1);
  ASSERT_TRUE(guest::WriteAll(partial, oversized.data(), oversized.size()));
  EXPECT_TRUE(closed(partial));
  ::close(partial);
  ::close(other);
}

TEST_F(BuildCpuSocket, OneUidCannotHoldEveryConnection) {
  std::vector<int> held;
  for (size_t i = 0; i < guest::kBuildCpuPeersPerUid; ++i) {
    held.push_back(connect());
    ASSERT_NE(ask(held.back()), nullptr);
  }
  const int refused = connect();
  EXPECT_EQ(ask(refused), nullptr);
  ::close(refused);
  ::close(held.back());
  held.pop_back();
  // A closed connection frees its place.
  const int next = connect();
  EXPECT_NE(ask(next), nullptr);
  held.push_back(next);
  for (const auto fd : held) ::close(fd);
}

}  // namespace
}  // namespace tsugumi::test
