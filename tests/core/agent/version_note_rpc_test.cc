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
  model.Queue(AnthropicText("Workflow の設定を変更しました。"));
  Configure(model, "anthropic");
  StartProject();
  const auto conversation = Conversation("version-notes");
  Grant(conversation, Tsugumi::Service::GrantScope::Conversation, Tsugumi::Service::Access::Read);
  Grant(conversation, Tsugumi::Service::GrantScope::Conversation, Tsugumi::Service::Access::Write);
  const auto initialized = ApprovedTool(conversation, "version_init", "{}");
  ASSERT_TRUE(initialized.Code().empty());
  const auto result = JsonString(initialized.View(), "/text");
  const auto commit = JsonString(result, "/value/commit");
  ASSERT_EQ(commit.size(), 12u);
  auto note = [&](client::Connection& connection, const std::string& language = "en",
                  const std::string& spelling = "") {
    return Call(connection, H::RequestPayload::VersionNoteRequest, [&](Builder& b) {
      return H::CreateVersionNoteRequest(b, b.CreateString(project_id), b.CreateString(spelling.empty() ? commit : spelling),
                                        b.CreateString("desktop"), b.CreateString(language)).Union();
    });
  };
  EXPECT_TRUE(note(*host).Code().empty());
  EXPECT_TRUE(note(*host).Code().empty());
  EXPECT_TRUE(model.Requests().empty());
  ASSERT_TRUE(SettingsPatch(*host, "desktop", {{"versions.noteMode", Quote("agent")}}).Code().empty());
  auto other = ConnectHost();
  auto uppercase = commit;
  std::transform(uppercase.begin(), uppercase.end(), uppercase.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  ASSERT_NE(uppercase, commit);
  auto first = std::async(std::launch::async, [&] { return note(*host); });
  const bool requested = model.WaitForRequests(1);
  if (!requested) model.Release();
  EXPECT_TRUE(requested);
  auto second = std::async(std::launch::async, [&] { return note(*other, "en", uppercase); });
  model.Release();
  const auto a = first.get(), b = second.get();
  ASSERT_TRUE(a.Code().empty()) << a.View();
  ASSERT_TRUE(b.Code().empty()) << b.View();
  EXPECT_EQ(a.HostValue()->value_as_TextResult()->text()->str(), "The workflow settings changed.");
  EXPECT_EQ(b.HostValue()->value_as_TextResult()->text()->str(), "The workflow settings changed.");
  EXPECT_EQ(model.Requests().size(), 1u);
  EXPECT_EQ(note(*host, "en", "HEAD").Code(), "invalid_request");
  EXPECT_EQ(JsonString(model.Requests().front(), "/model"), "claude-haiku-5-5");
  // Each display language owns its own generated summary.
  EXPECT_EQ(note(*host, "ja").HostValue()->value_as_TextResult()->text()->str(), "Workflow の設定を変更しました。");
  EXPECT_EQ(model.Requests().size(), 2u);
  ASSERT_TRUE(SettingsPatch(*host, "desktop", {{"versions.noteMode", Quote("mechanical")}}).Code().empty());
  EXPECT_TRUE(note(*host).Code().empty());
  ASSERT_TRUE(SettingsPatch(*host, "desktop", {{"versions.noteMode", Quote("agent")}}).Code().empty());
  EXPECT_EQ(note(*host).HostValue()->value_as_TextResult()->text()->str(), "The workflow settings changed.");
  EXPECT_EQ(model.Requests().size(), 2u);
  human.reset(); other.reset(); external.reset(); host.reset();
  service.Stop(); StartHost();
  EXPECT_EQ(note(*host).HostValue()->value_as_TextResult()->text()->str(), "The workflow settings changed.");
  EXPECT_EQ(model.Requests().size(), 2u);
}
TEST_F(AgentProjectFixture, HostGraphsRenderOneCommitInBothLanguagesAndAgentFormattingStaysOnHost) {
  StartProject();
  const auto conversation = Conversation("host-presentation");
  Grant(conversation, Tsugumi::Service::GrantScope::Conversation, Tsugumi::Service::Access::Read);
  Grant(conversation, Tsugumi::Service::GrantScope::Conversation, Tsugumi::Service::Access::Write);
  ASSERT_TRUE(ApprovedTool(conversation, "version_init", "{}").Code().empty());
  for (const auto& [language, expected] : std::vector<std::pair<std::string, std::string>>{
      {"en", "Create the project"}, {"ja", "プロジェクトを作成"}}) {
    const auto result = Call(*host, H::RequestPayload::HostVersionGraphRequest, [&](Builder& b) {
      return H::CreateHostVersionGraphRequest(b, b.CreateString(project_id), b.CreateString(language)).Union();
    });
    ASSERT_TRUE(result.Code().empty());
    const auto* graph = result.HostValue()->value_as_VersionGraph();
    ASSERT_NE(graph, nullptr);
    ASSERT_EQ(graph->commits()->size(), 1u);
    EXPECT_EQ(graph->commits()->Get(0)->message()->str(), expected);
  }
  const auto formatted = Tool(conversation, "project_file_format", R"({"path":"x.json","text":"{\"a\":1}"})", "format");
  ASSERT_TRUE(formatted.Code().empty());
  EXPECT_EQ(JsonString(JsonString(formatted.View(), "/text"), "/value/text"), "{\n  \"a\": 1\n}\n");
}
TEST_F(AgentProjectFixture, VersionNoteFailureCanBeRetriedAndEmptyDiffNeverCallsAModel) {
  ModelServer model;
  model.Queue(AnthropicText(""));
  model.Queue(AnthropicText("Workflow settings changed."));
  Configure(model);
  StartProject();
  const auto conversation = Conversation("version-note-retry");
  Grant(conversation, Tsugumi::Service::GrantScope::Conversation, Tsugumi::Service::Access::Read);
  Grant(conversation, Tsugumi::Service::GrantScope::Conversation, Tsugumi::Service::Access::Write);
  const auto initialized = ApprovedTool(conversation, "version_init", "{}");
  ASSERT_TRUE(initialized.Code().empty());
  auto commit = JsonString(JsonString(initialized.View(), "/text"), "/value/commit");
  ASSERT_TRUE(SettingsPatch(*host, "desktop", {{"versions.noteMode", Quote("agent")}}).Code().empty());
  const auto note = [&] {
    return Call(*host, H::RequestPayload::VersionNoteRequest, [&](Builder& b) {
      return H::CreateVersionNoteRequest(b, b.CreateString(project_id), b.CreateString(commit),
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
  EXPECT_EQ(note().HostValue()->value_as_TextResult()->text()->str(), "No Card definition, source, parameter or wiring changes");
  EXPECT_EQ(model.Requests().size(), 2u);
}
}  // namespace
}  // namespace tsugumi::test
