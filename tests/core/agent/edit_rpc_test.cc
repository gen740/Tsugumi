#include "project_fixture.hh"

namespace tsugumi::test {
namespace {
TEST_F(AgentProjectFixture,
       NarrowEditPreservesAllUntouchedProjectBytesAndReplaysTheToolCall) {
  StartProject();
  const auto conversation = Conversation("atomic-edit");
  Grant(conversation);
  Grant(conversation, Tsugumi::Service::GrantScope::Conversation,
        Tsugumi::Service::Access::Write);
  const auto before = Read(directory.path / "project/tsugumi.json");
  const std::string args =
      R"({"edits":[{"file_path":"tsugumi.json","selector":[{"field":"name"}],"old_value":{"present":true,"value":"PRIVATE_PROJECT_SENTINEL"},"new_value":{"present":true,"value":"Renamed"}}]})";
  const auto reply = Tool(conversation, "project_edit", args, "rename");
  ASSERT_TRUE(reply.Code().empty()) << reply.View();
  ASSERT_FALSE(JsonBool(reply.View(), "/isError")) << reply.View();
  auto expected = before;
  expected.replace(expected.find("PRIVATE_PROJECT_SENTINEL"),
                   std::string("PRIVATE_PROJECT_SENTINEL").size(), "Renamed");
  EXPECT_EQ(Read(directory.path / "project/tsugumi.json"), expected);
  const auto retry = Tool(conversation, "project_edit", args, "rename");
  ASSERT_TRUE(retry.Code().empty()) << retry.View();
  EXPECT_EQ(JsonString(reply.View(), "/text"),
            JsonString(retry.View(), "/text"));
  EXPECT_EQ(Read(directory.path / "project/tsugumi.json"), expected);
  const auto stale =
      Tool(conversation, "project_edit", args, "stale-old-value");
  ASSERT_TRUE(stale.Code().empty()) << stale.View();
  EXPECT_TRUE(JsonBool(stale.View(), "/isError"));
  EXPECT_EQ(Read(directory.path / "project/tsugumi.json"), expected);
}
TEST_F(AgentProjectFixture,
       NarrowFileEditCommitsTextReplacementAndGuardedMoveTogether) {
  StartProject();
  const auto conversation = Conversation("atomic-file-edit");
  Grant(conversation);
  Grant(conversation, Tsugumi::Service::GrantScope::Conversation,
        Tsugumi::Service::Access::Write);
  const auto created = Tool(
      conversation, "project_edit",
      R"({"edits":[{"file_path":"definitions/data/_card.json","put":"{\"template\":\"draft\"}","expect_missing":true},{"file_path":"definitions/data/file.txt","put":"old text","expect_missing":true}]})",
      "create");
  ASSERT_TRUE(created.Code().empty()) << created.View();
  ASSERT_FALSE(JsonBool(created.View(), "/isError")) << created.View();
  const auto edited = Tool(
      conversation, "project_edit",
      R"({"edits":[{"file_path":"definitions/data/file.txt","old_string":"old","new_string":"new"},{"file_path":"definitions/data/file.txt","move_to":"definitions/data/moved.txt","expected_hash":"cb0208b0b1fa06bc59f85c8b2be1e45ff2ef6ddbf0cef02e9f276b8208ea48ab","destination_missing":true}]})",
      "replace");
  ASSERT_TRUE(edited.Code().empty()) << edited.View();
  ASSERT_FALSE(JsonBool(edited.View(), "/isError")) << edited.View();
  EXPECT_EQ(Read(directory.path / "project/definitions/data/moved.txt"),
            "new text");
  EXPECT_FALSE(std::filesystem::exists(directory.path / "project/definitions/data/file.txt"));
}
}  // namespace
}  // namespace tsugumi::test
