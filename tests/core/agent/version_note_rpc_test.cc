// UI note requests own no provider configuration or project mutation logic.
#include "project_fixture.hh"
#include "oauth_fixture.hh"

#include <algorithm>
#include <cctype>

namespace tsugumi::test {
namespace {
TEST_F(AgentProjectFixture, VersionNotesDefaultToFactsAndShareOneSavedAgentSummary) {
  ModelServer model;
  model.Queue(AnthropicText("  The workflow settings\n changed.  "), true);
  // Let a regressed second generation finish, so it fails the count/text
  // assertions rather than waiting forever for another provider response.
  model.Queue(AnthropicText("Unexpected second summary."));
  Configure(model, "anthropic");
  StartProject();
  const auto conversation = Conversation("version-notes");
  Grant(conversation, A::GrantScope::Conversation, A::Access::Read);
  Grant(conversation, A::GrantScope::Conversation, A::Access::Write);
  const auto initialized = ApprovedTool(conversation, "version_init", "{}");
  ASSERT_TRUE(initialized.Code().empty());
  const auto result = JsonString(initialized.View(), "/text");
  const auto commit = JsonString(result, "/value/commit");
  ASSERT_EQ(commit.size(), 40u);
  auto note = [&](client::Connection& connection, const std::string& language = "en",
                  const std::string& spelling = "") {
    return Call(connection, A::RequestPayload::VersionNoteRequest, [&](Builder& b) {
      return A::CreateVersionNoteRequest(b, b.CreateString(project_id), b.CreateString(spelling.empty() ? commit : spelling),
                                        b.CreateString("desktop"), b.CreateString(language)).Union();
    });
  };
  EXPECT_EQ(note(*external).Code(), "permission_denied");
  EXPECT_EQ(note(*human).Value()->value_as_TextResult()->text()->str(), "Changed Workflow settings");
  EXPECT_EQ(note(*human, "ja").Value()->value_as_TextResult()->text()->str(), "Workflow の設定を変更");
  EXPECT_TRUE(model.Requests().empty());
  ASSERT_TRUE(SettingsPatch(*host, "desktop", {{"versions.noteMode", Quote("agent")}}).Code().empty());
  auto other = Connect(true);
  auto uppercase = commit;
  std::transform(uppercase.begin(), uppercase.end(), uppercase.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  ASSERT_NE(uppercase, commit);
  auto first = std::async(std::launch::async, [&] { return note(*human); });
  const bool requested = model.WaitForRequests(1);
  if (!requested) model.Release();
  EXPECT_TRUE(requested);
  auto second = std::async(std::launch::async, [&] { return note(*other, "en", uppercase); });
  EXPECT_EQ(second.wait_for(200ms), std::future_status::timeout);
  model.Release();
  const auto a = first.get(), b = second.get();
  ASSERT_TRUE(a.Code().empty()) << a.View();
  ASSERT_TRUE(b.Code().empty()) << b.View();
  EXPECT_EQ(a.Value()->value_as_TextResult()->text()->str(), "The workflow settings changed.");
  EXPECT_EQ(b.Value()->value_as_TextResult()->text()->str(), "The workflow settings changed.");
  EXPECT_EQ(model.Requests().size(), 1u);
  EXPECT_EQ(note(*human, "en", "HEAD").Code(), "invalid_request");
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
  const auto initialized = ApprovedTool(conversation, "version_init", "{}");
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
  const auto marked = ApprovedTool(conversation, "version_mark", R"({"name":"layout","message":"Layout"})");
  ASSERT_TRUE(marked.Code().empty());
  commit = JsonString(JsonString(marked.View(), "/text"), "/value/commit");
  ASSERT_TRUE(note().Code().empty());
  EXPECT_EQ(note().Value()->value_as_TextResult()->text()->str(), "No Card definition, source, parameter or wiring changes");
  EXPECT_EQ(model.Requests().size(), 2u);
}
}  // namespace
}  // namespace tsugumi::test
