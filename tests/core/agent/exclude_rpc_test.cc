#include "project_fixture.hh"

#include "agent/json.hh"
#include "util/crypto.hh"

namespace tsugumi::test {
namespace {
namespace j = agent::json;
class AgentExclusionRPC : public AgentProjectFixture {
protected:
  auto Error(const Reply &reply) -> std::string {
    if (!reply.Code().empty())
      return reply.Code();
    const auto raw = JsonString(reply.View(), "/text");
    if (!raw.starts_with('{'))
      return {};
    return j::Text(j::Object(j::Get(j::Object(raw), "value", "{}")), "kind");
  }
  auto Open(const std::vector<std::string> &excluded, bool shell = false)
      -> std::string {
    Write(directory.path / "project/definitions/public/_card.json",
          R"({"template":"draft"})");
    Write(directory.path / "project/definitions/hidden/_card.json",
          R"({"template":"draft"})");
    Write(directory.path / "project/definitions/hidden/src/secret.txt",
          "EXCLUDED_CONTENT_SENTINEL");
    std::vector<std::string> entries;
    for (const auto &path : excluded)
      entries.push_back(Quote(path));
    StartProject(shell, j::Encode(j::Members{
                            {"name", "\"Project\""},
                            {"pkgsPath", "\"tsugumi:default\""},
                            {"agentExclude", j::Encode(entries)},
                            {"canvas", R"({"cards":[],"connections":[]})"}}));
    const auto conversation = Conversation("exclusion");
    for (const auto access :
         {Tsugumi::Service::Access::Read, Tsugumi::Service::Access::Write})
      Grant(conversation, Tsugumi::Service::GrantScope::Conversation, access);
    return conversation;
  }
};
TEST_F(AgentExclusionRPC, FileReadListWriteAndPolicyChangesAreEnforcedByHost) {
  const auto c = Open({"definitions/hidden", ".git"});
  const auto listed =
      Tool(c, "project_file_list", R"({"path":"definitions","depth":8})");
  ASSERT_TRUE(listed.Code().empty());
  EXPECT_EQ(listed.View().find("hidden"), std::string::npos);
  EXPECT_EQ(Inspect(c).View().find("EXCLUDED_CONTENT_SENTINEL"),
            std::string::npos);
  EXPECT_EQ(Error(Tool(c, "project_file_read",
                       R"({"path":"definitions/hidden/src/secret.txt"})")),
            "not_found");
  EXPECT_EQ(
      Error(Tool(
          c, "project_edit",
          R"({"edits":[{"file_path":"definitions/hidden/new.txt","put":"forbidden","expect_missing":true}]})")),
      "not_found");
  EXPECT_EQ(
      Error(Tool(
          c, "project_edit",
          R"({"edits":[{"file_path":"tsugumi.json","selector":[{"field":"agentExclude"}],"old_value":{"present":true,"value":["definitions/hidden",".git"]},"new_value":{"present":true,"value":[]}}]})")),
      "permission_denied");
  EXPECT_EQ(
      Error(Tool(
          c, "project_save",
          R"({"base_rev":"0","changes":[{"value_type":"SetProject","value":{"json":"{\"name\":\"Project\",\"pkgsPath\":\"tsugumi:default\",\"agentExclude\":[],\"canvas\":{\"cards\":[],\"connections\":[]}}"}}]})")),
      "permission_denied");
  EXPECT_FALSE(std::filesystem::exists(directory.path /
                                       "project/definitions/hidden/new.txt"));
}
TEST_F(AgentExclusionRPC, DemoReadListCopyAndHelpersCannotReachExcludedDemo) {
  const auto demo = directory.path / "demos/ja/finished";
  Write(
      demo / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})");
  Write(demo / "definitions/step/_card.json", R"({"template":"draft"})");
  Write(demo / "definitions/step/src/note.txt", "EXCLUDED_DEMO_SENTINEL");
  Write(
      directory.path / "demos/ja/index.json",
      R"({"demos":[{"id":"finished","path":"finished","title":"EXCLUDED_TITLE"}]})");
  const auto c = Open({demo.string()});
  EXPECT_EQ(Tool(c, "demo_list", "{}").View().find("EXCLUDED_TITLE"),
            std::string::npos);
  EXPECT_EQ(
      Error(Tool(c, "demo_read",
                 R"({"path":"ja/finished/definitions/step/src/note.txt"})")),
      "not_found");
  EXPECT_EQ(
      Error(Tool(
          c, "project_copy",
          R"({"source":"demo","demo":"ja/finished","card":"step","id":"copied","base_rev":"0"})")),
      "not_found");
  ModelServer model;
  model.Queue(AnthropicTool("list", "demo_list", "{}"));
  model.Queue(
      AnthropicTool("read", "demo_read",
                    R"({"path":"ja/finished/definitions/step/src/note.txt"})"));
  model.Queue(AnthropicText("Done"));
  Configure(model);
  const auto result = Tool(
      c, "delegate",
      R"({"tasks":[{"name":"reader","instruction":"Read the bundled examples.","read_cards":[],"write_cards":[],"run_cards":[]}]})");
  ASSERT_TRUE(result.Code().empty()) << result.Code();
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_EQ(requests.back().find("EXCLUDED_DEMO_SENTINEL"), std::string::npos);
  EXPECT_EQ(requests.back().find("EXCLUDED_TITLE"), std::string::npos);
  EXPECT_NE(requests.back().find("not_found"), std::string::npos);
}
TEST_F(AgentExclusionRPC, DemoCardCopySavesItsDefinitionAndSources) {
  const auto demo = directory.path / "demos/en/source";
  Write(
      demo / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})");
  Write(demo / "definitions/step/_card.json", R"({"template":"draft"})");
  Write(demo / "definitions/step/_meta.json",
        R"({"name":"Source step","description":"Keep description"})");
  Write(demo / "definitions/step/src/note.txt", "COPIED_COMPLETE_SOURCE");
  Write(directory.path / "demos/en/index.json",
        R"({"demos":[{"id":"source","path":"source","title":"Source"}]})");
  const auto c = Open({".git"});
  const auto result = Tool(
      c, "project_copy",
      R"({"source":"demo","demo":"en/source","card":"step","id":"copied","base_rev":"0"})");
  ASSERT_EQ(Error(result), "") << result.View();
  EXPECT_EQ(Read(directory.path / "project/definitions/copied/src/note.txt"),
            "COPIED_COMPLETE_SOURCE");
  EXPECT_NE(Read(directory.path / "project/definitions/copied/_meta.json")
                .find("Keep description"),
            std::string::npos);
}
TEST_F(AgentExclusionRPC, AttachedAliasesCannotReadOrCopyExcludedSources) {
  const auto attached = directory.path / "attached";
  Write(attached / "secret.txt", "EXCLUDED_ATTACHMENT");
  const auto c = Open({(attached / "secret.txt").string()});
  ASSERT_TRUE(Call(*host, H::RequestPayload::RegisterFolderRequest,
                   [&](Builder &b) {
                     return H::CreateRegisterFolderRequest(
                                b, b.CreateString(project_id),
                                b.CreateString("folder:excluded"),
                                b.CreateString(attached.string()))
                         .Union();
                   })
                  .Code()
                  .empty());
  const auto args = j::Encode(
      j::Members{{"folder", Quote(attached.string())}, {"path", Quote("")}});
  const auto pending = Tool(c, "attached_folder_read", args);
  ASSERT_EQ(pending.Code(), "grant_required");
  ASSERT_TRUE(Call(*human, A::RequestPayload::AccessResolveRequest,
                   [&](Builder &b) {
                     return A::CreateAccessResolveRequest(
                                b, b.CreateString(pending.RequestId()), true)
                         .Union();
                   })
                  .Code()
                  .empty());
  EXPECT_EQ(Tool(c, "attached_folder_read", args).View().find("secret.txt"),
            std::string::npos);
  EXPECT_EQ(
      Error(Tool(c, "attached_folder_read",
                 j::Encode(j::Members{{"folder", Quote(attached.string())},
                                      {"path", Quote("secret.txt")}}))),
      "not_found");
  EXPECT_EQ(
      Error(Tool(
          c, "project_copy",
          "{\"source\":\"attached\",\"folder\":" + Quote(attached.string()) +
              R"(,"base_rev":"0","files":[{"from":"secret.txt","to":"definitions/public/copied.txt"}]})")),
      "not_found");
}
// This profile regression must run outside the agent's development sandbox.
// Keep it separate from the other host tests so it can be selected explicitly.
TEST_F(AgentExclusionRPC,
       ShellProfileDeniesExcludedProjectDemoAndMountedPaths) {
  if (::geteuid() == 0) GTEST_SKIP() << "Shell commands require an unprivileged project user";
  const auto demo = directory.path / "demos/en/finished";
  Write(demo / "secret.txt", "EXCLUDED_DEMO");
  Write(directory.path / "project/.git/config", "EXCLUDED_GIT");
  const auto c = Open({".git", "definitions/hidden", demo.string()}, true);
  for (const auto access :
       {Tsugumi::Service::Access::Run, Tsugumi::Service::Access::Shell})
    Grant(c, Tsugumi::Service::GrantScope::Conversation, access);
  // Attaching their common ancestor is the route most likely to bypass a
  // workspace-only allowlist. The explicit deny must survive this mount.
  const auto attached = directory.path;
  ASSERT_TRUE(Call(*host, H::RequestPayload::RegisterFolderRequest,
                   [&](Builder &b) {
                     return H::CreateRegisterFolderRequest(
                                b, b.CreateString(project_id),
                                b.CreateString("folder:root"),
                                b.CreateString(attached.string()))
                         .Union();
                   })
                  .Code()
                  .empty());
  const auto pending =
      Tool(c, "attached_folder_read",
           j::Encode(j::Members{{"folder", Quote(attached.string())},
                                {"path", Quote("")}}));
  ASSERT_EQ(pending.Code(), "grant_required");
  ASSERT_TRUE(Call(*human, A::RequestPayload::AccessResolveRequest,
                   [&](Builder &b) {
                     return A::CreateAccessResolveRequest(
                                b, b.CreateString(pending.RequestId()), true)
                         .Union();
                   })
                  .Code()
                  .empty());
  const auto mount =
      "attached/" + crypto::sha256_hex(attached.string()).substr(0, 16) + '/';
  const auto command = "cat definitions/hidden/src/secret.txt " +
                       Quote((demo / "secret.txt").string()) + ' ' + mount +
                       "project/.git/config " + mount +
                       "demos/en/finished/secret.txt; true";
  const auto result = Tool(c, "project_shell",
                           j::Encode(j::Members{{"command", Quote(command)}}));
  ASSERT_EQ(Error(result), "") << result.View();
  const auto output =
      JsonString(JsonString(result.View(), "/text"), "/value/stdout_bytes");
  EXPECT_EQ(output.find("EXCLUDED_"), std::string::npos);
}
} // namespace
} // namespace tsugumi::test
