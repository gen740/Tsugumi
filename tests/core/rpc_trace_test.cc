#include <gtest/gtest.h>

#include "tsugumi/protocol/host_generated.h"
#include "tsugumi/protocol/remote_worker_generated.h"
#include "tsugumi/protocol/trace_codec.hh"

namespace tsugumi::trace {
namespace {
namespace H = Tsugumi::Host;
namespace R = Tsugumi::RemoteWorker;
namespace P = Tsugumi::RPC;
namespace S = Tsugumi::Service;

auto Bytes(flatbuffers::FlatBufferBuilder& builder) -> std::string {
  return {reinterpret_cast<const char*>(builder.GetBufferPointer()),
          builder.GetSize()};
}

template <class T>
auto Request(T value, uint64_t id = 7) -> std::string {
  flatbuffers::FlatBufferBuilder builder;
  H::RequestT request;
  request.value.Set(std::move(value));
  H::MessageT message;
  message.request_id = id;
  message.payload.Set(std::move(request));
  H::FinishMessageBuffer(builder, H::Message::Pack(builder, &message));
  return Bytes(builder);
}

auto FailureReply() -> std::string {
  flatbuffers::FlatBufferBuilder builder;
  S::FailureT error;
  error.code = "invalid_request";
  error.message = "No such request";
  H::ReplyT reply;
  reply.value.Set(std::move(error));
  H::MessageT message;
  message.request_id = 7;
  message.payload.Set(std::move(reply));
  H::FinishMessageBuffer(builder, H::Message::Pack(builder, &message));
  return Bytes(builder);
}

class RpcTrace : public testing::Test {
 protected:
  void SetUp() override {
    SetMode(Mode::Off);
    const auto snapshot = Read();
    if (!snapshot.entries.empty()) after_ = snapshot.entries.back().sequence;
  }

  void TearDown() override {
    CancelWatch();
    SetMode(Mode::Off);
  }

  auto ReadNew(std::function<void()> changed = {}) const -> Snapshot {
    return Read(std::move(changed), after_);
  }

  auto Entries() const -> std::vector<Entry> { return ReadNew().entries; }

  auto ObserveFrame(std::string_view frame, bool reply = false) -> void {
    Observe(link_, reply ? "hostd" : "macOS app", reply ? "macOS app" : "hostd",
            reply, frame, &kFlatBufferCodec);
  }

 private:
  Link link_;
  uint64_t after_ = 0;
};

TEST_F(RpcTrace, OffRecordsNothing) {
  ObserveFrame(Request(H::HostActivityRequestT{}));
  EXPECT_TRUE(Entries().empty());
}

TEST_F(RpcTrace, TraceRecordsMetadataAndCorrelatesRepliesWithoutContent) {
  SetMode(Mode::Trace);
  const auto request = Request(H::HostActivityRequestT{});
  ObserveFrame(request);
  ObserveFrame(FailureReply(), true);

  const auto entries = Entries();
  ASSERT_EQ(entries.size(), 2);
  EXPECT_EQ(entries[0].from, "macOS app");
  EXPECT_EQ(entries[0].to, "hostd");
  EXPECT_EQ(entries[0].direction, Direction::Request);
  EXPECT_EQ(entries[0].type, "HostActivityRequest");
  EXPECT_EQ(entries[0].request_id, 7);
  EXPECT_EQ(entries[0].bytes, request.size());
  EXPECT_GT(entries[0].unix_micros, 0);
  EXPECT_TRUE(entries[0].content.json.empty());
  EXPECT_EQ(entries[1].direction, Direction::Reply);
  EXPECT_EQ(entries[1].type, "Failure");
  EXPECT_EQ(entries[1].request_id, 7);
  EXPECT_EQ(entries[1].failure, "invalid_request");
  EXPECT_TRUE(entries[1].elapsed_micros.has_value());
  EXPECT_TRUE(entries[1].content.json.empty());
}

TEST_F(RpcTrace, SnapshotWatchWakesOnceUntilTheNextRead) {
  SetMode(Mode::Trace);
  int changes = 0;
  const auto changed = [&changes] { ++changes; };
  ReadNew(changed);

  ObserveFrame(Request(H::HostActivityRequestT{}, 1));
  ObserveFrame(Request(H::HostActivityRequestT{}, 2));
  EXPECT_EQ(changes, 1);

  const auto snapshot = ReadNew(changed);
  ASSERT_EQ(snapshot.entries.size(), 2);
  EXPECT_EQ(snapshot.entries.back().request_id, 2);

  ObserveFrame(Request(H::HostActivityRequestT{}, 3));
  EXPECT_EQ(changes, 2);
  const auto delta = Read({}, snapshot.entries.back().sequence);
  ASSERT_EQ(delta.entries.size(), 1);
  EXPECT_EQ(delta.entries.front().request_id, 3);
}

TEST_F(RpcTrace, CaptureDecodesAndClearsContentOnModeChange) {
  SetMode(Mode::Capture);
  H::SettingsGetRequestT settings;
  settings.profile = "desktop";
  ObserveFrame(Request(settings));
  auto entries = Entries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_NE(entries[0].content.json.find("\"profile\": \"desktop\""),
            std::string::npos);
  const auto captured = entries.back().sequence;
  SetMode(Mode::Trace);
  const auto delta = Read({}, captured);
  EXPECT_TRUE(delta.entries.empty());
  EXPECT_EQ(delta.cleared_through, captured);
  entries = Entries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_TRUE(entries[0].content.json.empty());
  EXPECT_EQ(entries[0].type, "SettingsGetRequest");

  SetMode(Mode::Capture);
  ObserveFrame(Request(settings));
  SetMode(Mode::Off);
  EXPECT_TRUE(Entries().back().content.json.empty());
}

TEST_F(RpcTrace, CaptureDecodesSparseProjectUnionTags) {
  SetMode(Mode::Capture);
  flatbuffers::FlatBufferBuilder builder;
  const auto value = P::CreateProjectActivityRequest(builder);
  const auto request = P::CreateRequest(
      builder, P::RequestPayload::ProjectActivityRequest, value.Union());
  P::FinishMessageBuffer(
      builder,
      P::CreateMessage(builder, 1, P::Payload::Request, request.Union()));
  Link link;
  Observe(link, "macOS app", "project core", false, Bytes(builder),
          &kFlatBufferCodec);

  const auto entries = Entries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_NE(entries[0].content.json.find("ProjectActivityRequest"),
            std::string::npos);
}

TEST_F(RpcTrace, CaptureRedactsTokensNestedCredentialsAndOpaqueSettings) {
  SetMode(Mode::Capture);
  S::HelloRequestT hello;
  hello.token = "private-host-token";
  hello.client = Tsugumi::Trace::Client::MacOS;
  ObserveFrame(Request(hello));

  H::ProviderSetRequestT provider;
  provider.profile = "desktop";
  provider.id = "openai";
  provider.provider = std::make_unique<H::ProviderInputT>();
  provider.provider->api_key = "private-api-key";
  provider.provider->model = "test-model";
  ObserveFrame(Request(provider));

  H::SettingsUpdateRequestT settings;
  settings.profile = "desktop";
  auto change = std::make_unique<H::SettingT>();
  change->key = "credential";
  change->value_json = "\"opaque-secret\"";
  settings.changes.push_back(std::move(change));
  ObserveFrame(Request(settings));

  const auto entries = Entries();
  ASSERT_EQ(entries.size(), 3);
  EXPECT_NE(entries[0].content.json.find("MacOS"), std::string::npos);
  EXPECT_NE(entries[1].content.json.find("test-model"), std::string::npos);
  EXPECT_NE(entries[2].content.json.find("credential"), std::string::npos);
  EXPECT_EQ(entries[0].content.json.find("private-host-token"),
            std::string::npos);
  EXPECT_EQ(entries[1].content.json.find("private-api-key"), std::string::npos);
  EXPECT_EQ(entries[2].content.json.find("opaque-secret"), std::string::npos);
  for (const auto& entry : entries)
    EXPECT_STREQ(entry.content.omission, "redacted");
}

TEST_F(RpcTrace, RemoteBrokerCapabilityIsRedacted) {
  SetMode(Mode::Capture);
  R::BrokerRequestT broker;
  broker.capability = "private-broker-capability";
  broker.request = std::make_unique<R::RequestT>();
  broker.request->core_life_id = "private-core-lease";
  broker.request->body.Set(R::ProbeRequestT{});
  R::MessageT message;
  message.payload.Set(std::move(broker));
  flatbuffers::FlatBufferBuilder builder;
  R::FinishMessageBuffer(builder, R::Message::Pack(builder, &message));
  Link link;
  Observe(link, "project core", "hostd execution broker", true, Bytes(builder),
          &kFlatBufferCodec);

  const auto entries = Entries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].type, "ProbeRequest");
  EXPECT_FALSE(entries[0].request_id);
  EXPECT_NE(entries[0].content.json.find("ProbeRequest"), std::string::npos);
  EXPECT_EQ(entries[0].content.json.find("private-"), std::string::npos);
  EXPECT_STREQ(entries[0].content.omission, "redacted");
}

TEST_F(RpcTrace, CapsMetadataAndContentIndependently) {
  SetMode(Mode::Trace);
  for (uint64_t id = 1; id <= kMaxEntries + 1; ++id)
    ObserveFrame(Request(H::HostActivityRequestT{}, id));
  auto entries = Entries();
  ASSERT_EQ(entries.size(), kMaxEntries);
  EXPECT_EQ(entries.front().request_id, 2);
  EXPECT_EQ(Read({}, entries.back().sequence).first_sequence,
            entries.front().sequence);

  SetMode(Mode::Capture);
  H::SettingsGetRequestT settings;
  settings.profile = std::string(32u << 10, 'a');
  const auto frame = Request(settings);
  for (size_t i = 0; i <= kMaxContentBytes / settings.profile.size(); ++i)
    ObserveFrame(frame);
  entries = Entries();
  ASSERT_EQ(entries.size(), kMaxEntries);
  size_t total = 0;
  bool evicted = false;
  for (const auto& entry : entries) {
    total += entry.content.json.size();
    evicted |= std::string_view(entry.content.omission) == "evicted";
  }
  EXPECT_LE(total, kMaxContentBytes);
  EXPECT_TRUE(evicted);
  const auto delta = Read({}, entries.back().sequence);
  EXPECT_TRUE(delta.entries.empty());
  EXPECT_GT(delta.evicted_through, delta.cleared_through);
  EXPECT_FALSE(entries.back().content.json.empty());

  settings.profile.resize(kMaxFrameToDecode + 1, 'a');
  ObserveFrame(Request(settings));
  entries = Entries();
  EXPECT_STREQ(entries.back().content.omission, "size");
  EXPECT_TRUE(entries.back().content.json.empty());
}

}  // namespace
}  // namespace tsugumi::trace
