#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "tsugumi/rpc/frame.h"
#include "tsugumi/rpc/frame.hh"

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
}  // namespace
}  // namespace tsugumi::rpc::frame
