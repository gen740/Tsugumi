#include <arpa/inet.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <string>

#include "tsugumi/rpc/client_transport.hh"
#include "tsugumi/rpc/frame.h"
#include "tsugumi/rpc/frame.hh"
#include "tsugumi/util/crypto.hh"

namespace tsugumi::rpc::frame {
namespace {
constexpr size_t kLimit = 64;

class FrameTest : public ::testing::Test {
 protected:
  void SetUp() override {
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    reader_ = pair[0];
    writer_ = pair[1];
  }
  void TearDown() override {
    if (reader_ >= 0) ::close(reader_);
    CloseWriter();
  }
  void CloseWriter() {
    if (writer_ >= 0) ::close(writer_);
    writer_ = -1;
  }
  void Send(uint32_t length, std::string_view body = {}) {
    const auto prefix = EncodePrefix(length);
    std::string bytes(prefix.data(), prefix.size());
    bytes += body;
    ASSERT_EQ(::write(writer_, bytes.data(), bytes.size()),
              static_cast<ssize_t>(bytes.size()));
  }
  auto Kind(auto read) -> Error::Kind {
    try {
      read();
    } catch (const Error& error) {
      return error.kind;
    }
    ADD_FAILURE() << "no frame error";
    return Error::Kind::IO;
  }
  int reader_ = -1;
  int writer_ = -1;
};

TEST_F(FrameTest, BodyIoErrorLeavesOutputsEmpty) {
  ASSERT_EQ(::fcntl(reader_, F_SETFL, O_NONBLOCK), 0);
  Send(10, "abc");
  uint8_t* bytes = nullptr;
  uint32_t size = 0;
  EXPECT_EQ(tsugumi_frame_read(reader_, kLimit, &bytes, &size),
            TSUGUMI_FRAME_IO);
  EXPECT_EQ(bytes, nullptr);
  EXPECT_EQ(size, 0u);
}

TEST_F(FrameTest, PrefixIsLittleEndian) {
  const auto prefix = EncodePrefix(0x01020304);
  EXPECT_EQ(std::string(prefix.data(), prefix.size()), "\x04\x03\x02\x01");
  EXPECT_EQ(DecodePrefix(prefix.data(), UINT32_MAX), 0x01020304u);
}

TEST_F(FrameTest, AFrameExactlyAtTheLimitPasses) {
  const std::string payload(kLimit, 'x');
  Write(writer_, payload, kLimit);
  EXPECT_EQ(Read(reader_, kLimit), payload);
  CloseWriter();
  EXPECT_EQ(Read(reader_, kLimit), std::nullopt);
}

TEST_F(FrameTest, OneByteOverTheLimitIsRejectedBeforeItsBody) {
  // No body follows and the writer stays open: reading one would block.
  Send(kLimit + 1);
  EXPECT_EQ(Kind([&] { Read(reader_, kLimit); }), Error::Kind::Size);
  Send(UINT32_MAX);
  EXPECT_EQ(Kind([&] { Read(reader_, kLimit); }), Error::Kind::Size);
  EXPECT_EQ(Kind([&] { Write(writer_, std::string(kLimit + 1, 'x'), kLimit); }),
            Error::Kind::Size);
}

TEST_F(FrameTest, AnEmptyFrameIsRejected) {
  Send(0);
  EXPECT_EQ(Kind([&] { Read(reader_, kLimit); }), Error::Kind::Size);
  EXPECT_EQ(Kind([&] { Write(writer_, "", kLimit); }), Error::Kind::Size);
}

TEST_F(FrameTest, ATruncatedFrameIsAnError) {
  Send(10, "abc");
  CloseWriter();
  EXPECT_EQ(Kind([&] { Read(reader_, kLimit); }), Error::Kind::Truncated);
}

TEST_F(FrameTest, ATruncatedPrefixIsAnError) {
  ASSERT_EQ(::write(writer_, "\x07\x00", 2), 2);
  CloseWriter();
  EXPECT_EQ(Kind([&] { Read(reader_, kLimit); }), Error::Kind::Truncated);
}

TEST_F(FrameTest, TheCInterfaceAppliesTheSameRules) {
  const std::string payload(kLimit, 'y');
  ASSERT_EQ(tsugumi_frame_write(writer_,
                                reinterpret_cast<const uint8_t*>(payload.data()),
                                kLimit, kLimit),
            TSUGUMI_FRAME_OK);
  uint8_t* bytes = nullptr;
  uint32_t size = 0;
  ASSERT_EQ(tsugumi_frame_read(reader_, kLimit, &bytes, &size),
            TSUGUMI_FRAME_OK);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(bytes), size), payload);
  tsugumi_frame_free(bytes);

  EXPECT_EQ(tsugumi_frame_write(writer_, nullptr, kLimit + 1, kLimit),
            TSUGUMI_FRAME_SIZE);
  Send(kLimit + 1);
  EXPECT_EQ(tsugumi_frame_read(reader_, kLimit, &bytes, &size),
            TSUGUMI_FRAME_SIZE);
  EXPECT_EQ(bytes, nullptr);
  Send(10, "abc");
  CloseWriter();
  EXPECT_EQ(tsugumi_frame_read(reader_, kLimit, &bytes, &size),
            TSUGUMI_FRAME_TRUNCATED);
  EXPECT_EQ(tsugumi_frame_read(reader_, kLimit, &bytes, &size),
            TSUGUMI_FRAME_END);
}
class NativeWebSocketTest : public testing::Test {
 protected:
  auto SetUp() -> void override {
    listener = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address),
                     sizeof(address)),
              0);
    ASSERT_EQ(::listen(listener, 1), 0);
    socklen_t size = sizeof(address);
    ASSERT_EQ(
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size),
        0);
  }

  auto TearDown() -> void override {
    if (listener >= 0) ::close(listener);
  }

  auto Endpoint() const -> std::string {
    return "ws://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/rpc";
  }

  auto Serve(bool valid, bool expect_pong = true) -> std::future<bool> {
    return std::async(std::launch::async, [this, valid, expect_pong] {
      pollfd waiting{listener, POLLIN, 0};
      if (::poll(&waiting, 1, 5000) != 1) return false;
      const int fd = ::accept(listener, nullptr, nullptr);
      if (fd < 0) return false;
      struct Peer {
        int fd;
        ~Peer() { ::close(fd); }
      } peer{fd};
      const timeval timeout{5, 0};
      ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      std::string request;
      while (request.find("\r\n\r\n") == request.npos &&
             request.size() < 4096) {
        char ch;
        if (::read(fd, &ch, 1) != 1) return false;
        request += ch;
      }
      const std::string_view name = "Sec-WebSocket-Key: ";
      const auto start = request.find(name);
      if (start == request.npos) return false;
      const auto value = start + name.size();
      const auto key =
          request.substr(value, request.find("\r\n", value) - value);
      if (!valid) {
        const std::string response = "HTTP/1.1 403 Forbidden\r\n\r\n";
        return ::write(fd, response.data(), response.size()) ==
               static_cast<ssize_t>(response.size());
      }
      const auto digest =
          crypto::sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
      auto response =
          "HTTP/1.1 101 Switching Protocols\r\n"
          "Connection: keep-alive, Upgrade\r\nUpgrade: websocket\r\n"
          "Sec-WebSocket-Protocol: test\r\nSec-WebSocket-Accept: " +
          crypto::base64(digest.data(), digest.size()) + "\r\n\r\n";
      // Two fragments with an interleaved Ping.
      response +=
          "\x02\x02"
          "ab"
          "\x89\x01"
          "p"
          "\x80\x01"
          "c";
      if (::write(fd, response.data(), response.size()) !=
          static_cast<ssize_t>(response.size()))
        return false;
      if (!expect_pong) {
        char header[2];
        return ReadExact(fd, header, sizeof(header)) == sizeof(header) &&
               static_cast<uint8_t>(header[0]) == 0x82;
      }
      char pong[7];
      return ReadExact(fd, pong, sizeof(pong)) == sizeof(pong) &&
             static_cast<uint8_t>(pong[0]) == 0x8A &&
             static_cast<uint8_t>(pong[1]) == 0x81 &&
             (pong[2] ^ pong[6]) == 'p';
    });
  }

 private:
  int listener = -1;
  sockaddr_in address{};
};

TEST_F(NativeWebSocketTest, RejectsHttpErrorBeforeOpening) {
  using namespace std::chrono_literals;
  std::promise<client::Error> closed;
  auto completion = closed.get_future();
  std::atomic<bool> opened = false;
  auto peer = Serve(false);
  auto transport = client::MakeTransport({Endpoint(), "test"});
  client::TransportEvents events;
  events.opened = [&] { opened = true; };
  events.closed = [&](client::Error error) {
    closed.set_value(std::move(error));
  };
  transport->Start(std::move(events));
  ASSERT_EQ(completion.wait_for(5s), std::future_status::ready);
  EXPECT_FALSE(opened);
  EXPECT_EQ(completion.get().code, client::ErrorCode::Transport);
  EXPECT_TRUE(peer.get());
}

TEST_F(NativeWebSocketTest, ReassemblesFragmentsAndAnswersPing) {
  using namespace std::chrono_literals;
  std::promise<client::Bytes> received;
  auto message = received.get_future();
  auto peer = Serve(true);
  auto transport = client::MakeTransport({Endpoint(), "test"});
  client::TransportEvents events;
  events.received = [&](client::Bytes bytes) {
    received.set_value(std::move(bytes));
  };
  transport->Start(std::move(events));
  ASSERT_EQ(message.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(message.get(), (client::Bytes{'a', 'b', 'c'}));
  EXPECT_TRUE(peer.get());
}

}  // namespace
}  // namespace tsugumi::rpc::frame
