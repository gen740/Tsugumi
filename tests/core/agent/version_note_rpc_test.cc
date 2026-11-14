// UI note requests own no provider configuration or project mutation logic.
#include "project_fixture.hh"
#include "oauth_fixture.hh"

namespace tsugumi::test {
namespace {
TEST_F(AgentProjectFixture, VersionNotesDefaultToFactsAndShareOneSavedAgentSummary) {
  ModelServer model;
  model.Queue(AnthropicText("  The workflow settings\n changed.  "));
  Configure(model, "anthropic");
  StartProject();
  const auto conversation = Conversation("version-notes");
  Grant(conversation, A::GrantScope::Conversation, A::Access::Read);
  Grant(conversation, A::GrantScope::Conversation, A::Access::Write);
  const auto initialized = Tool(conversation, "version_init", "{}");
  ASSERT_TRUE(initialized.Code().empty());
  const auto result = JsonString(initialized.View(), "/text");
  const auto commit = JsonString(result, "/value/commit");
  ASSERT_EQ(commit.size(), 40u);
  auto note = [&](client::Connection& connection, const std::string& language = "en") {
    return Call(connection, A::RequestPayload::VersionNoteRequest, [&](Builder& b) {
      return A::CreateVersionNoteRequest(b, b.CreateString(project_id), b.CreateString(commit),
                                        b.CreateString("desktop"), b.CreateString(language)).Union();
    });
  };
  EXPECT_EQ(note(*external).Code(), "permission_denied");
  EXPECT_EQ(note(*human).Value()->value_as_TextResult()->text()->str(), "Changed Workflow settings");
  EXPECT_EQ(note(*human, "ja").Value()->value_as_TextResult()->text()->str(), "Workflow の設定を変更");
  EXPECT_TRUE(model.Requests().empty());
  ASSERT_TRUE(SettingsPatch(*host, "desktop", {{"versions.noteMode", Quote("agent")}}).Code().empty());
  auto other = Connect(true);
  auto first = std::async(std::launch::async, [&] { return note(*human); });
  auto second = std::async(std::launch::async, [&] { return note(*other); });
  const auto a = first.get(), b = second.get();
  ASSERT_TRUE(a.Code().empty()) << a.View();
  ASSERT_TRUE(b.Code().empty()) << b.View();
  EXPECT_EQ(a.Value()->value_as_TextResult()->text()->str(), "The workflow settings changed.");
  EXPECT_EQ(b.Value()->value_as_TextResult()->text()->str(), "The workflow settings changed.");
  EXPECT_EQ(model.Requests().size(), 1u);
  EXPECT_EQ(JsonString(model.Requests().front(), "/model"), "claude-haiku-5-5");
  // A cache miss in a client or another language cannot make a second model call.
  EXPECT_EQ(note(*human, "ja").Value()->value_as_TextResult()->text()->str(), "The workflow settings changed.");
  EXPECT_EQ(model.Requests().size(), 1u);
  ASSERT_TRUE(SettingsPatch(*host, "desktop", {{"versions.noteMode", Quote("mechanical")}}).Code().empty());
  EXPECT_EQ(note(*human).Value()->value_as_TextResult()->text()->str(), "Changed Workflow settings");
  ASSERT_TRUE(SettingsPatch(*host, "desktop", {{"versions.noteMode", Quote("agent")}}).Code().empty());
  EXPECT_EQ(note(*human).Value()->value_as_TextResult()->text()->str(), "The workflow settings changed.");
  EXPECT_EQ(model.Requests().size(), 1u);
  human.reset(); other.reset(); external.reset(); host.reset();
  agent.Stop(); StartAgent();
  EXPECT_EQ(note(*human).Value()->value_as_TextResult()->text()->str(), "The workflow settings changed.");
  EXPECT_EQ(model.Requests().size(), 1u);
}
TEST_F(AgentProjectFixture, VersionNoteFailureCanBeRetriedAndEmptyDiffNeverCallsAModel) {
  ModelServer model;
  model.Queue(AnthropicText(""));
  model.Queue(AnthropicText("Workflow settings changed."));
  Configure(model);
  StartProject();
  const auto conversation = Conversation("version-note-retry");
  Grant(conversation, A::GrantScope::Conversation, A::Access::Read);
  Grant(conversation, A::GrantScope::Conversation, A::Access::Write);
  const auto initialized = Tool(conversation, "version_init", "{}");
  ASSERT_TRUE(initialized.Code().empty());
  auto commit = JsonString(JsonString(initialized.View(), "/text"), "/value/commit");
  ASSERT_TRUE(SettingsPatch(*host, "desktop", {{"versions.noteMode", Quote("agent")}}).Code().empty());
  const auto note = [&] {
    return Call(*human, A::RequestPayload::VersionNoteRequest, [&](Builder& b) {
      return A::CreateVersionNoteRequest(b, b.CreateString(project_id), b.CreateString(commit),
                                        b.CreateString("desktop"), b.CreateString("en")).Union();
    });
  };
  EXPECT_EQ(note().Code(), "unavailable");
  EXPECT_TRUE(note().Code().empty());
  EXPECT_EQ(model.Requests().size(), 2u);
  Write(directory.path / "project/layout.json", R"({"flow":"topToBottom"})");
  const auto marked = Tool(conversation, "version_mark", R"({"name":"layout","message":"Layout"})");
  ASSERT_TRUE(marked.Code().empty());
  commit = JsonString(JsonString(marked.View(), "/text"), "/value/commit");
  ASSERT_TRUE(note().Code().empty());
  EXPECT_EQ(note().Value()->value_as_TextResult()->text()->str(), "No Card definition, source, parameter or wiring changes");
  EXPECT_EQ(model.Requests().size(), 2u);
}
}  // namespace
}  // namespace tsugumi::test
