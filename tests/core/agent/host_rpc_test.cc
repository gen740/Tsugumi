// Public Host contract scenarios, independent of project/conversation
// lifetimes.
#include "fixture.hh"
#include <unistd.h>

namespace tsugumi::test {
namespace {
TEST_F(AgentFixture, HostUnixAndWebSocketReturnTypedReplies) {
  auto websocket = ConnectHost(true);
  const auto unix_reply =
      Call(*host, H::RequestPayload::HostRecentListRequest, [](Builder &b) {
        return H::CreateHostRecentListRequest(b).Union();
      });
  const auto ws_reply = Call(
      *websocket, H::RequestPayload::HostRecentListRequest,
      [](Builder &b) { return H::CreateHostRecentListRequest(b).Union(); });
  ASSERT_NE(unix_reply.HostValue()->value_as_RecentList(), nullptr);
  ASSERT_NE(ws_reply.HostValue()->value_as_RecentList(), nullptr);
  EXPECT_EQ(unix_reply.View(), ws_reply.View());
  const auto conversation = Conversation("typed", true);
  const auto reply = Get(conversation, true);
  ASSERT_NE(reply.Value()->value_as_ConversationResult(), nullptr);
  EXPECT_EQ(
      reply.Value()->value_as_ConversationResult()->conversation()->id()->str(),
      conversation);
}
TEST_F(AgentFixture, HostRejectsTheExternalAgentCredentialOnBothTransports) {
  for (const bool websocket : {false, true})
    EXPECT_NO_THROW(ConnectHost(websocket, false, false));
}
TEST_F(AgentFixture, HostFramesAreRejectedByTheAgentListener) {
  client::Options options;
  options.protocol = client::Protocol::Host;
  options.endpoint = "unix:" + JsonString(ready, "/unix_socket");
  client::Connection wrong(options);
  ASSERT_FALSE(wrong.Start().get());
  EXPECT_THROW(
      Call(wrong, H::RequestPayload::HelloRequest,
           [&](Builder &b) {
             return Tsugumi::Service::CreateHelloRequest(
                        b, b.CreateString(JsonString(ready, "/user_token")),
                        b.CreateString("wrong"))
                 .Union();
           }),
      std::runtime_error);
  EXPECT_TRUE(agent.Alive());
}
// Host lifecycle scenarios need no project daemon and no conversation.
class HostProjectsFixture : public AgentFixture {
protected:
  auto Create(const std::string &name, const std::string &flow = "topToBottom")
      -> Reply {
    return Call(*host, H::RequestPayload::HostProjectCreateRequest,
                [&](Builder &b) {
                  return H::CreateHostProjectCreateRequest(
                             b, b.CreateString(directory.path.string()),
                             b.CreateString(name), b.CreateString(flow))
                      .Union();
                });
  }
  auto Remember(const std::filesystem::path &path) -> Reply {
    return Call(*host, H::RequestPayload::HostRecentRememberRequest,
                [&](Builder &b) {
                  return H::CreateHostRecentRememberRequest(
                             b, b.CreateString(path.string()))
                      .Union();
                });
  }
  auto Recents() -> Reply {
    return Call(
        *human, H::RequestPayload::HostRecentListRequest,
        [](Builder &b) { return H::CreateHostRecentListRequest(b).Union(); });
  }
};

TEST_F(HostProjectsFixture, CreatesPinnedDocumentsInNewAndExactlyEmptyFolders) {
  const auto defaults = Call(
      *human, H::RequestPayload::HostProjectDefaultsRequest, [](Builder &b) {
        return H::CreateHostProjectDefaultsRequest(b).Union();
      });
  ASSERT_TRUE(Create("new", "leftToRight").Code().empty());
  const auto folder = directory.path / "new";
  EXPECT_EQ(JsonString(Read(folder / "tsugumi.json"), "/pkgsPath"),
            JsonString(defaults.View(), "/nixpkgsPin"));
  EXPECT_EQ(JsonString(Read(folder / "layout.json"), "/flow"), "leftToRight");
  std::filesystem::create_directory(directory.path / "empty");
  EXPECT_TRUE(Create("empty").Code().empty());
  EXPECT_EQ(Create("new").Code(), "folder_not_empty");
  Write(directory.path / "occupied/sentinel", "keep");
  EXPECT_EQ(Create("occupied").Code(), "folder_not_empty");
  EXPECT_EQ(Read(directory.path / "occupied/sentinel"), "keep");
  // Only Finder's own file: still an empty folder.
  Write(directory.path / "finder/.DS_Store", "finder");
  EXPECT_TRUE(Create("finder").Code().empty());
  EXPECT_TRUE(std::filesystem::exists(directory.path / "finder/tsugumi.json"));
  EXPECT_EQ(Create("bad-flow", "sideways").Code(), "invalid_flow");
  EXPECT_FALSE(std::filesystem::exists(directory.path / "bad-flow"));
  for (const auto *name :
       {"", ".", "..", "a/b", "a\\b", "trailing ", "bad:name"})
    EXPECT_EQ(Create(name).Code(), "invalid_project_name") << name;
  const auto info =
      Call(*host, H::RequestPayload::HostProjectInfoRequest, [&](Builder &b) {
        return H::CreateHostProjectInfoRequest(b,
                                               b.CreateString(folder.string()))
            .Union();
      });
  EXPECT_EQ(JsonString(info.View(), "/name"), "new");
}

TEST_F(HostProjectsFixture,
       CopiesDemosAndProjectsWithoutStateAndWithWritableFiles) {
  const auto source = directory.path / "demos/ja/sample";
  Write(source / "tsugumi.json", R"({"name":"Sample"})");
  Write(source / "definitions/card/src/main.py", "print('sample')");
  Write(source / ".tsugumi/history", "private run");
  Write(source / "nested/.tsugumi/state", "private nested state");
  Write(source / ".git/config", "old repository");
  Write(
      directory.path / "demos/ja/index.json",
      R"({"demos":[{"id":"sample","title":"Sample","path":"sample","field":""}]})");
  const auto list =
      Call(*host, H::RequestPayload::HostDemoListRequest, [](Builder &b) {
        return H::CreateHostDemoListRequest(b, b.CreateString("ja")).Union();
      });
  EXPECT_EQ(JsonString(list.View(), "/0/id"), "sample");
  EXPECT_EQ(JsonString(list.View(), "/0/description"), "");
  EXPECT_NE(list.View().find("\"tags\":[]"), std::string::npos);
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(source))
    std::filesystem::permissions(entry.path(),
                                 entry.is_directory()
                                     ? std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_exec
                                     : std::filesystem::perms::owner_read);
  std::filesystem::permissions(source, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_exec);
  const auto copy = [&](const std::string &id) {
    return Call(*host, H::RequestPayload::HostDemoCopyRequest, [&](Builder &b) {
      return H::CreateHostDemoCopyRequest(
                 b, b.CreateString(id), b.CreateString(directory.path.string()),
                 b.CreateString("ja"))
          .Union();
    });
  };
  const auto first = copy("sample");
  const auto second = copy("sample");
  EXPECT_EQ(JsonString(first.View(), "/path"),
            (std::filesystem::canonical(directory.path) / "sample").string());
  EXPECT_EQ(JsonString(second.View(), "/path"),
            (std::filesystem::canonical(directory.path) / "sample-2").string());
  EXPECT_EQ(copy("missing").Code(), "demo_not_found");
  const auto duplicate = Call(
      *human, H::RequestPayload::HostProjectDuplicateRequest, [&](Builder &b) {
        return H::CreateHostProjectDuplicateRequest(
                   b, b.CreateString(source.string()),
                   b.CreateString(directory.path.string()),
                   b.CreateString("duplicate"))
            .Union();
      });
  ASSERT_TRUE(duplicate.Code().empty());
  for (const auto *name : {"sample", "duplicate"}) {
    const auto dest = directory.path / name;
    EXPECT_FALSE(std::filesystem::exists(dest / ".tsugumi"));
    EXPECT_FALSE(std::filesystem::exists(dest / "nested/.tsugumi"));
    EXPECT_FALSE(std::filesystem::exists(dest / ".git"));
    EXPECT_EQ(::access((dest / "definitions/card/src/main.py").c_str(), W_OK),
              0);
    Write(dest / "definitions/card/src/added.py", "editable");
  }
  // Restore the fixture so its cleanup can remove read-only source folders.
  std::filesystem::permissions(source, std::filesystem::perms::owner_all);
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(source))
    std::filesystem::permissions(entry.path(),
                                 std::filesystem::perms::owner_all);
}

TEST_F(HostProjectsFixture,
       RegistryHasThirtyNewestUniqueProjectsAndSurvivesRestart) {
  for (int i = 0; i < 32; ++i) {
    const auto name = "recent-" + std::to_string(i);
    ASSERT_TRUE(Create(name).Code().empty());
    ASSERT_TRUE(Remember(directory.path / name).Code().empty());
  }
  auto list = Recents().View();
  EXPECT_EQ(JsonString(list, "/0/name"), "recent-31");
  EXPECT_EQ(JsonString(list, "/29/name"), "recent-2");
  EXPECT_EQ(list.find("recent-1\""), std::string::npos);
  EXPECT_THROW((void)JsonString(list, "/30/name"), std::runtime_error);
  ASSERT_TRUE(Remember(directory.path / "recent-4").Code().empty());
  list = Recents().View();
  EXPECT_EQ(JsonString(list, "/0/name"), "recent-4");
  EXPECT_GT(JsonUInt(list, "/0/openedAt"), JsonUInt(list, "/1/openedAt"));
  const auto forgotten =
      Call(*host, H::RequestPayload::HostRecentForgetRequest, [&](Builder &b) {
        const std::vector<flatbuffers::Offset<flatbuffers::String>> paths{
            b.CreateString((directory.path / "recent-4").string())};
        return H::CreateHostRecentForgetRequest(b, b.CreateVector(paths))
            .Union();
      });
  EXPECT_TRUE(forgotten.Code().empty());
  EXPECT_EQ(JsonString(Recents().View(), "/0/name"), "recent-31");
  EXPECT_TRUE(
      std::filesystem::exists(directory.path / "recent-4/tsugumi.json"));
  const auto before = Recents().View();
  external.reset();
  human.reset();
  agent.Stop();
  StartAgent();
  EXPECT_EQ(Recents().View(), before);
  std::filesystem::remove_all(directory.path / "recent-31");
  EXPECT_EQ(JsonString(Recents().View(), "/0/name"), "recent-30");
}

} // namespace
} // namespace tsugumi::test
