// The same flatc goldens are consumed by the Swift and Web service adapters.
#include <gtest/gtest.h>

#include "agent/json.hh"
#include "agent/wire.hh"
#include "util/fs.hh"

namespace tsugumi::host {
namespace {
namespace A = Tsugumi::Agent;
namespace H = Tsugumi::Host;
namespace S = Tsugumi::Service;
namespace json = agent::json;

auto Bytes(const std::string& hex) -> std::string {
  std::string bytes;
  for (size_t i = 0; i < hex.size(); i += 2)
    bytes.push_back(
        static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return bytes;
}

TEST(ServiceContracts, SharedGoldensKeepRequestsOnTheirOwningListener) {
  const auto fixture =
      json::Object(fs::read_file_or_throw(TSUGUMI_SERVICE_FIXTURES));
  for (const auto& raw : json::Array(json::Get(fixture, "requests"))) {
    const auto c = json::Object(raw);
    const auto method = json::Text(c, "method");
    SCOPED_TRACE(method);
    const auto bytes = Bytes(json::Text(c, "hex"));
    const auto fields = json::Object(json::Get(c, "fields"));
    flatbuffers::Verifier verifier(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    if (json::Bool(json::Get(c, "host"))) {
      ASSERT_TRUE(H::VerifyMessageBuffer(verifier));
      EXPECT_FALSE(A::MessageBufferHasIdentifier(bytes.data()));
      const auto* message = H::GetMessage(bytes.data());
      EXPECT_EQ(message->request_id(), 7U);
      const auto* request = message->payload_as_Request();
      ASSERT_NE(request, nullptr);
      EXPECT_EQ(H::EnumNameRequestPayload(request->value_type()), method);
      if (const auto* read = request->value_as_DemoReadRequest())
        EXPECT_EQ(read->path()->str(), json::Text(fields, "path"));
      if (const auto* note = request->value_as_VersionNoteRequest()) {
        EXPECT_EQ(note->project_id()->str(), json::Text(fields, "project_id"));
        EXPECT_EQ(note->commit()->str(), json::Text(fields, "commit"));
        EXPECT_EQ(note->profile()->str(), json::Text(fields, "profile"));
        EXPECT_EQ(note->language()->str(), json::Text(fields, "language"));
        EXPECT_EQ(note->anthropic_key_profile()->str(),
                  json::Text(fields, "anthropic_key_profile"));
        EXPECT_EQ(note->openai_key_profile()->str(),
                  json::Text(fields, "openai_key_profile"));
      }
    } else {
      ASSERT_TRUE(A::VerifyMessageBuffer(verifier));
      EXPECT_FALSE(H::MessageBufferHasIdentifier(bytes.data()));
      const auto* message = A::GetMessage(bytes.data());
      EXPECT_EQ(message->request_id(), 7U);
      const auto* request = message->payload_as_Request();
      ASSERT_NE(request, nullptr);
      EXPECT_EQ(A::EnumNameRequestPayload(request->value_type()), method);
      if (const auto* access = request->value_as_AccessRequest()) {
        EXPECT_EQ(S::EnumNameAccess(access->access()),
                  json::Text(fields, "access"));
        EXPECT_EQ(S::EnumNameGrantScope(access->scope()),
                  json::Text(fields, "scope"));
      }
      if (const auto* summary =
              request->value_as_ConversationSummarizeRequest()) {
        EXPECT_EQ(summary->conversation_id()->str(),
                  json::Text(fields, "conversation_id"));
        ASSERT_NE(summary->provider(), nullptr);
        EXPECT_EQ(summary->provider()->purpose()->str(), "summary");
      }
    }
  }
}

TEST(ServiceContracts, OperationGoldensAllowEmptyOptionalEditVectors) {
  const auto fixture =
      json::Object(fs::read_file_or_throw(TSUGUMI_SERVICE_FIXTURES));
  for (const auto& raw : json::Array(json::Get(fixture, "operationRequests"))) {
    const auto bytes = Bytes(json::Text(json::Object(raw), "hex"));
    flatbuffers::Verifier verifier(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    ASSERT_TRUE(H::VerifyMessageBuffer(verifier));
    const auto* input = H::GetMessage(bytes.data())
                            ->payload_as_Request()
                            ->value_as_ProjectOperateRequest();
    ASSERT_NE(input, nullptr);
    std::unique_ptr<H::ProjectOperateRequestT> object(input->UnPack());
    ASSERT_TRUE(object->body.AsProjectEdits()->edits.empty());
    H::MessageT message;
    H::RequestT request;
    request.value.Set(*object);
    message.payload.Set(std::move(request));
    flatbuffers::FlatBufferBuilder builder;
    H::FinishMessageBuffer(builder, H::Message::Pack(builder, &message));
    flatbuffers::Verifier roundtrip(builder.GetBufferPointer(),
                                    builder.GetSize());
    EXPECT_TRUE(H::VerifyMessageBuffer(roundtrip));
  }
  for (const auto& raw : json::Array(json::Get(fixture, "operationReplies"))) {
    const auto bytes = Bytes(json::Text(json::Object(raw), "hex"));
    flatbuffers::Verifier verifier(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    ASSERT_TRUE(H::VerifyMessageBuffer(verifier));
    const auto* result = H::GetMessage(bytes.data())
                             ->payload_as_Reply()
                             ->value_as_ProjectOperationResult();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->reply()->value_as_SaveResult()->rev().value_or(0), 2u);
    EXPECT_EQ(result->reply()->value_as_SaveResult()->delta(), nullptr);
  }
}

TEST(ServiceContracts, BothRepliesUseOneTextResultAndHostListsMayBeOmitted) {
  S::TextResultT text;
  text.text = "summary";
  text.usage = std::make_unique<S::UsageT>();
  text.usage->input_tokens = 3;
  const auto agent = agent::wire::AgentReply(7, text);
  const auto host = agent::wire::HostReply(7, text);
  const auto* a =
      A::GetMessage(agent.data())->payload_as_Reply()->value_as_TextResult();
  const auto* h =
      H::GetMessage(host.data())->payload_as_Reply()->value_as_TextResult();
  ASSERT_NE(a, nullptr);
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(a->text()->str(), h->text()->str());
  EXPECT_EQ(a->usage()->input_tokens(), h->usage()->input_tokens());
  const auto empty = agent::wire::HostReply(8, H::DemoPathsT{});
  flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(empty.data()),
                                 empty.size());
  ASSERT_TRUE(H::VerifyMessageBuffer(verifier));
  const auto* paths =
      H::GetMessage(empty.data())->payload_as_Reply()->value_as_DemoPaths();
  ASSERT_NE(paths, nullptr);
  EXPECT_EQ(paths->paths(), nullptr);
}
}  // namespace
}  // namespace tsugumi::host
