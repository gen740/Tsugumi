// Version notes are the change record; the Agent reads a version diff only
// when a person asks.
#include <algorithm>
#include <cctype>

#include "oauth_fixture.hh"
#include "project_fixture.hh"

namespace tsugumi::test {
namespace {
TEST_F(AgentProjectFixture,
       VersionNotesAreTheChangeRecordAndTheAgentReadsTheDiffWhenAsked) {
  StartProject(
      false,
      R"({"name":"PRIVATE_PROJECT_SENTINEL","agentExclude":[],"nixPath": [],"canvas":{"cards":[],"connections":[]}})");
  const auto conversation = Conversation("version-notes");
  const auto initialized = ApprovedTool(conversation, "version_init", "{}");
  ASSERT_TRUE(initialized.Code().empty());
  const auto first =
      JsonString(JsonString(initialized.View(), "/text"), "/value/commit");
  Write(directory.path / "project/layout.bin", FixtureLayout(R"({"flow":"topToBottom"})"));
  const auto marked = ApprovedTool(conversation, "version_mark",
                                   R"({"name":"layout","message":"Layout"})");
  ASSERT_TRUE(marked.Code().empty());
  const auto commit =
      JsonString(JsonString(marked.View(), "/text"), "/value/commit");
  auto note = [&](client::Connection& connection, const std::string& language,
                  const std::string& spelling) {
    return Call(
        connection, H::RequestPayload::VersionNoteRequest, [&](Builder& b) {
          return H::CreateVersionNoteRequest(b, b.CreateString(project_id),
                                             b.CreateString(spelling),
                                             b.CreateString(language))
              .Union();
        });
  };
  // A shorter spelling always differs, even when the hash has only digits.
  auto alternate = commit.substr(0, 7);
  std::transform(
      alternate.begin(), alternate.end(), alternate.begin(),
      [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  ASSERT_NE(alternate, commit);
  const auto english = note(*host, "en", commit);
  ASSERT_TRUE(english.Code().empty()) << english.View();
  EXPECT_EQ(english.HostValue()->value_as_TextResult()->text()->str(),
            "No Card definition, source, parameter or wiring changes");
  const auto other = note(*host, "en", alternate);
  ASSERT_TRUE(other.Code().empty()) << other.View();
  EXPECT_EQ(other.HostValue()->value_as_TextResult()->text()->str(),
            english.HostValue()->value_as_TextResult()->text()->str());
  const auto japanese = note(*host, "ja", commit);
  ASSERT_TRUE(japanese.Code().empty());
  EXPECT_NE(japanese.HostValue()->value_as_TextResult()->text()->str(),
            english.HostValue()->value_as_TextResult()->text()->str());
  EXPECT_EQ(note(*host, "en", "HEAD").Code(), "invalid_request");
  const auto diff = [&](const std::string& args) {
    const auto reply = Tool(conversation, "version_diff", args);
    EXPECT_TRUE(reply.Code().empty()) << reply.View();
    const auto value = JsonString(reply.View(), "/text");
    EXPECT_EQ(JsonString(value, "/reply"), "VersionDiff");
    return JsonString(value, "/value/text");
  };
  EXPECT_NE(diff(R"({"commit":")" + first + R"("})").find("tsugumi.json"),
            std::string::npos);
  // layout.bin says where Cards sit, not what they do.
  EXPECT_EQ(diff(R"({"commit":")" + commit + R"("})"), "");
  EXPECT_EQ(diff(R"({"commit":")" + commit + R"(","from_commit":")" + first +
                 R"("})"),
            "");
}
TEST_F(AgentProjectFixture,
       HostGraphsRenderOneCommitInBothLanguagesAndAgentFormattingStaysOnHost) {
  StartProject(
      false,
      R"({"name":"PRIVATE_PROJECT_SENTINEL","agentExclude":[],"nixPath": [],"canvas":{"cards":[],"connections":[]}})");
  const auto conversation = Conversation("host-presentation");
  ASSERT_TRUE(ApprovedTool(conversation, "version_init", "{}").Code().empty());
  for (const auto& [language, expected] :
       std::vector<std::pair<std::string, std::string>>{
           {"en", "Create the project"}, {"ja", "プロジェクトを作成"}}) {
    const auto result = Call(
        *host, H::RequestPayload::HostVersionGraphRequest, [&](Builder& b) {
          return H::CreateHostVersionGraphRequest(b, b.CreateString(project_id),
                                                  b.CreateString(language))
              .Union();
        });
    ASSERT_TRUE(result.Code().empty());
    const auto* graph = result.HostValue()->value_as_VersionGraph();
    ASSERT_NE(graph, nullptr);
    ASSERT_EQ(graph->commits()->size(), 1u);
    EXPECT_EQ(graph->commits()->Get(0)->message()->str(), expected);
  }
  const auto formatted =
      Tool(conversation, "project_file_format",
           R"({"path":"x.json","text":"{\"a\":1}"})", "format");
  ASSERT_TRUE(formatted.Code().empty());
  EXPECT_EQ(JsonString(JsonString(formatted.View(), "/text"), "/value/text"),
            "{\n  \"a\": 1\n}\n");
}
}  // namespace
}  // namespace tsugumi::test
