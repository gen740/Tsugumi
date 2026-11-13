// Skill discovery and selection use only the public Host/Agent contracts.
#include <sys/stat.h>

#include "project_fixture.hh"

namespace tsugumi::test {
namespace {
class SkillsFixture : public AgentProjectFixture {
 protected:
  auto List(const std::vector<std::string>& folders = {}, bool project = true,
            bool websocket = false, const std::string& conversation = {})
      -> Reply {
    auto connection = websocket ? ConnectHost(true) : nullptr;
    return Call(
        connection ? *connection : *host, H::RequestPayload::SkillsListRequest,
        [&](Builder& b) {
          std::vector<flatbuffers::Offset<flatbuffers::String>> sources;
          for (const auto& folder : folders)
            sources.push_back(b.CreateString(folder));
          return H::CreateSkillsListRequest(
                     b,
                     project
                         ? b.CreateString((directory.path / "project").string())
                         : 0,
                     b.CreateVector(sources), b.CreateString(conversation))
              .Union();
        });
  }
};
TEST_F(SkillsFixture, NoProjectStillHasDemoAndMissingFoldersHaveStatus) {
  const auto empty = List({}, false);
  ASSERT_TRUE(empty.Code().empty());
  EXPECT_EQ(ListSize(empty.HostValue()->value_as_SkillsList()->skills()), 1u);
  EXPECT_EQ(ListSize(empty.HostValue()->value_as_SkillsList()->folders()), 0u);
  const auto missing =
      List({(directory.path / "missing").string()}, false, true);
  const auto* result = missing.HostValue()->value_as_SkillsList();
  ASSERT_EQ(ListSize(result->folders()), 1u);
  EXPECT_EQ(result->folders()->Get(0)->status()->str(), "unreadable");
}
TEST_F(SkillsFixture, FrontMatterFallbackNamesAndSwiftSortOrder) {
  const auto root = directory.path / "project/skills";
  Write(root / "a.md",
        "---\nname: Card review!\ndescription: Review "
        "outputs\n---\nBODY_SENTINEL");
  Write(root / "B/SKILL.md", "# Explain runs #\nRead outputs");
  Write(root / "c.MD", "---\nname: unfinished\n# No closing delimiter");
  Write(root / "notes.txt", "ignored");
  const auto reply = List();
  const auto* list = reply.HostValue()->value_as_SkillsList()->skills();
  ASSERT_EQ(ListSize(list), 4u);
  EXPECT_EQ(list->Get(0)->path()->str(), "builtin:demo");
  EXPECT_EQ(list->Get(1)->name()->str(), "B");  // Swift <, not localeCompare.
  EXPECT_EQ(list->Get(1)->description()->str(), "Explain runs");
  EXPECT_EQ(list->Get(2)->name()->str(), "Card-review-");
  EXPECT_EQ(list->Get(2)->description()->str(), "Review outputs");
  EXPECT_EQ(list->Get(3)->name()->str(), "c");
  EXPECT_EQ(list->Get(3)->description()->str(), "---");
  EXPECT_EQ(
      std::string(reply.bytes.begin(), reply.bytes.end()).find("BODY_SENTINEL"),
      std::string::npos);
}
TEST_F(SkillsFixture, SwiftUnicodeNamesWhitespaceAndCRLFBehavior) {
  const auto root = directory.path / "project/skills";
  Write(
      root / "a.md",
      "---\nname: \u00a0e\u0301-Ⅳ-¼-👨‍👩‍👦\u200b\ndescription: "
      "\u00a0Review\u200b\n---\nbody");
  Write(root / "b.md", "---\r\nname: ignored\r\n---\r\nbody");
  Write(root / "c.md",
        "---\nname: bad" + std::string("\xff", 1) + "name\n---\nbody");
  const auto reply = List();
  const auto* skills = reply.HostValue()->value_as_SkillsList()->skills();
  ASSERT_EQ(ListSize(skills), 4u);
  EXPECT_EQ(skills->Get(1)->name()->str(), "é-Ⅳ-¼--");
  EXPECT_EQ(skills->Get(1)->description()->str(), "Review");
  EXPECT_EQ(skills->Get(2)->name()->str(), "b");
  EXPECT_EQ(skills->Get(2)->description()->str(),
            "---\r\nname: ignored\r\n---\r\nbody");
  EXPECT_EQ(skills->Get(3)->name()->str(), "bad-name");
}

TEST_F(SkillsFixture, StrictSizeBoundaryAndUnreadableFilesAndFolders) {
  const auto root = directory.path / "project/skills";
  Write(root / "below.md", std::string(512 * 1024 - 1, 'x'));
  Write(root / "exact.md", std::string(512 * 1024, 'x'));
  Write(root / "over.md", std::string(512 * 1024 + 1, 'x'));
  Write(root / "unreadable.md", "private");
  const auto folder = directory.path / "locked";
  Write(folder / "skill.md", "private");
  ASSERT_EQ(::chmod((root / "unreadable.md").c_str(), 0000), 0);
  ASSERT_EQ(::chmod(folder.c_str(), 0000), 0);
  const auto reply = List({folder.string()});
  ::chmod((root / "unreadable.md").c_str(), 0600);
  ::chmod(folder.c_str(), 0700);
  ASSERT_TRUE(reply.Code().empty());
  const auto* result = reply.HostValue()->value_as_SkillsList();
  const auto* list = result->skills();
  ASSERT_EQ(ListSize(list), 5u);
  EXPECT_EQ(list->Get(1)->status()->str(), "ready");
  EXPECT_EQ(list->Get(2)->status()->str(), "too_large");
  EXPECT_EQ(list->Get(3)->status()->str(), "too_large");
  EXPECT_EQ(list->Get(4)->status()->str(), "unreadable");
  EXPECT_EQ(result->folders()->Get(1)->status()->str(), "unreadable");
}
TEST_F(SkillsFixture,
       CollisionsRemainInSourceOrderAndDirectorySymlinksAreNotFollowed) {
  const auto root = directory.path / "project/skills";
  const auto first = directory.path / "first";
  const auto second = directory.path / "second";
  for (const auto& dir : {root, first, second})
    Write(dir / "same.md", "---\nname: same\n---\nbody");
  Write(first / "real/SKILL.md", "# linked");
  std::filesystem::create_directory_symlink(first / "real",
                                            first / "directory-link");
  std::filesystem::create_symlink(first / "same.md", first / "file-link.md");
  const auto reply = List({first.string(), second.string()});
  const auto* list = reply.HostValue()->value_as_SkillsList()->skills();
  ASSERT_EQ(ListSize(list), 6u);
  EXPECT_EQ(list->Get(1)->source_folder()->str(), root.string());
  EXPECT_EQ(list->Get(2)->path()->str(), (first / "file-link.md").string());
  EXPECT_EQ(list->Get(3)->name()->str(), "real");
  EXPECT_EQ(list->Get(4)->name()->str(), "same");
  EXPECT_EQ(list->Get(5)->source_folder()->str(), second.string());
}
TEST_F(SkillsFixture,
       NamesSelectEveryCollisionAndSourcesBelongToTheConversation) {
  ModelServer model;
  model.Queue(AnthropicText("First selected skills received."));
  model.Queue(AnthropicText("Second selected skills received."));
  Configure(model);
  const auto first = directory.path / "project/first";
  const auto second = directory.path / "project/second";
  Write(first / "a.md", "---\nname: shared\n---\nFIRST_A_SENTINEL");
  Write(first / "b.md", "---\nname: shared\n---\nFIRST_B_SENTINEL");
  Write(second / "a.md", "---\nname: shared\n---\nSECOND_SENTINEL");
  StartProject();
  const auto one = Conversation("first-catalog");
  const auto two = Conversation("second-catalog");
  ASSERT_TRUE(List({first.string()}, true, false, one).Code().empty());
  ASSERT_TRUE(List({second.string()}, true, false, two).Code().empty());
  Grant(one);
  Grant(two);
  const auto send = [&](const std::string& conversation) {
    return Call(
        *external, A::RequestPayload::ConversationSendRequest, [&](Builder& b) {
          return A::CreateConversationSendRequest(
                     b, b.CreateString(conversation),
                     b.CreateString("selected"),
                     b.CreateString("Use the selected skill"), 0, 0,
                     b.CreateVector(std::vector{b.CreateString("shared")}))
              .Union();
        });
  };
  ASSERT_TRUE(send(one).Code().empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  WaitUntilIdle(one);
  ASSERT_TRUE(send(two).Code().empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  WaitUntilIdle(two);
  const auto requests = model.Requests();
  EXPECT_NE(requests[0].find("FIRST_A_SENTINEL"), std::string::npos);
  EXPECT_NE(requests[0].find("FIRST_B_SENTINEL"), std::string::npos);
  EXPECT_LT(requests[0].find("FIRST_A_SENTINEL"),
            requests[0].find("FIRST_B_SENTINEL"));
  EXPECT_EQ(requests[0].find("SECOND_SENTINEL"), std::string::npos);
  EXPECT_NE(requests[1].find("SECOND_SENTINEL"), std::string::npos);
  EXPECT_EQ(requests[1].find("FIRST_A_SENTINEL"), std::string::npos);
}

TEST_F(AgentProjectFixture,
       CustomSkillContentsNeedPermissionBeforeEnteringTheModelPrompt) {
  ModelServer model;
  model.Queue(AnthropicText("I received the authorized skill."));
  Configure(model);
  Write(directory.path / "project/skills/private-skill.md",
        "PRIVATE_SKILL_SENTINEL\nFollow the user's selected skill.");
  StartProject();
  const auto discovery =
      Call(*host, H::RequestPayload::SkillsListRequest, [&](Builder& b) {
        return H::CreateSkillsListRequest(
                   b, b.CreateString((directory.path / "project").string()))
            .Union();
      });
  ASSERT_TRUE(discovery.Code().empty());
  const auto conversation = Conversation("custom-skill-permission");
  auto selected_skill = [&] {
    return Call(
        *external, A::RequestPayload::ConversationSendRequest, [&](Builder& b) {
          const auto skills =
              b.CreateVector(std::vector{b.CreateString("private-skill")});
          return A::CreateConversationSendRequest(
                     b, b.CreateString(conversation),
                     b.CreateString("selected-skill"),
                     b.CreateString("Use my selected skill."), 0, 0, skills)
              .Union();
        });
  };
  ASSERT_TRUE(selected_skill().Code().empty());
  EXPECT_FALSE(model.WaitForRequests(1, 100ms));
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (Events(conversation).View().find("permission") == std::string::npos &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_NE(Events(conversation).View().find("permission"), std::string::npos);
  external.reset();
  human.reset();
  agent.Stop(true);
  StartAgent();
  const auto interrupted = Get(conversation).View();
  EXPECT_TRUE(JsonBool(interrupted, "/resumable"));
  EXPECT_TRUE(JsonBool(interrupted, "/continuable"));
  ASSERT_TRUE(Call(*external, A::RequestPayload::ConversationActionRequest,
                   [&](Builder& b) {
                     return A::CreateConversationActionRequest(
                                b, b.CreateString(conversation),
                                A::Action::Resume)
                         .Union();
                   })
                  .Code()
                  .empty());
  EXPECT_FALSE(model.WaitForRequests(1, 100ms));
  const auto grant = Grant(conversation);
  ASSERT_TRUE(model.WaitForRequests(1));
  const auto completed = WaitUntilIdle(conversation);
  EXPECT_FALSE(JsonBool(completed, "/resumable"));
  EXPECT_FALSE(JsonBool(completed, "/continuable"));
  EXPECT_EQ(model.Requests().size(), 1u);
  EXPECT_NE(model.Requests()[0].find("PRIVATE_SKILL_SENTINEL"),
            std::string::npos);
  Revoke(grant);
  EXPECT_EQ(Send(conversation, "Continue with that skill.", "turn-2").Code(),
            "permission_denied");
}
}  // namespace
}  // namespace tsugumi::test
