// Layout tools are hosted by agentd; only the final edit reaches the project.
#include "project_fixture.hh"

namespace tsugumi::test {
namespace {
TEST_F(AgentProjectFixture, LayoutProposalsUseTheHostAndDoNotSaveTheProject) {
  StartProject();
  const auto conversation = Conversation("host-layout-proposal");
  Grant(conversation);
  const auto before = JsonString(Inspect(conversation).View(), "/text");
  const auto reply = Tool(
      conversation, "layout_arrange",
      R"({"scene":{"flow":"topToBottom","cards":[],"wires":[],"frames":[],"notes":[]}})",
      "arrange");
  ASSERT_TRUE(reply.Code().empty()) << reply.View();
  EXPECT_FALSE(JsonBool(reply.View(), "/isError"));
  EXPECT_EQ(JsonString(JsonString(reply.View(), "/text"), "/reply"),
            "LayoutProposal");
  EXPECT_EQ(JsonString(Inspect(conversation).View(), "/text"), before);
}

TEST_F(AgentProjectFixture,
       HostLayoutRejectionReplaysWithoutLookingForACoreReceipt) {
  StartProject();
  const auto conversation = Conversation("host-layout-validation");
  Grant(conversation);
  Grant(conversation, A::GrantScope::Conversation, A::Access::Write);
  const auto before = JsonString(Inspect(conversation).View(), "/text");
  const auto revision = JsonString(before, "/value/rev");
  const auto arguments =
      "{\"base_rev\":" + Quote(revision) +
      R"(,"changes":[{"value_type":"SetLayout","value":{"json":"{\"cards\":{\"a\":{\"x\":1}}}"}}]})";
  const auto refused =
      Tool(conversation, "project_save", arguments, "bad-layout");
  ASSERT_TRUE(refused.Code().empty()) << refused.View();
  ASSERT_TRUE(JsonBool(refused.View(), "/isError"));
  const auto result = JsonString(refused.View(), "/text");
  EXPECT_EQ(JsonString(result, "/value/kind"), "invalid_document");
  const auto retry =
      Tool(conversation, "project_save", arguments, "bad-layout");
  ASSERT_TRUE(retry.Code().empty()) << retry.View();
  EXPECT_EQ(JsonString(retry.View(), "/text"), result);
  EXPECT_EQ(JsonString(Inspect(conversation).View(), "/text"), before);
}
} // namespace
} // namespace tsugumi::test
