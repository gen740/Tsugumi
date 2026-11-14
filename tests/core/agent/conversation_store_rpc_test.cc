#include <sqlite3.h>

#include "project_fixture.hh"

namespace tsugumi::test {
namespace {

// Inspect the public on-disk boundary, without linking Agent implementation
// APIs.
auto Scalar(const std::filesystem::path& path, const std::string& sql)
    -> int64_t {
  sqlite3* raw = nullptr;
  const auto opened =
      sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw,
                                                              sqlite3_close);
  if (opened != SQLITE_OK)
    throw std::runtime_error("cannot inspect " + path.string());
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(raw, sql.c_str(), -1, &statement, nullptr) !=
      SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(raw));
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(
      statement, sqlite3_finalize);
  if (sqlite3_step(statement) != SQLITE_ROW)
    throw std::runtime_error(sqlite3_errmsg(raw));
  return sqlite3_column_int64(statement, 0);
}

class ConversationStoreFixture : public AgentProjectFixture {
 protected:
  auto Register(const std::filesystem::path& folder,
                client::Connection& connection) -> Reply {
    return Call(
        connection, H::RequestPayload::RegisterProjectRequest, [&](Builder& b) {
          return H::CreateRegisterProjectRequest(
                     b, b.CreateString(project_id),
                     b.CreateString("unix:" +
                                    JsonString(project_ready, "/unix_socket")),
                     b.CreateString(JsonString(project_ready, "/token")),
                     b.CreateString("Project conversations"),
                     b.CreateString(folder.string()))
              .Union();
        });
  }
  auto StopAgent() -> void {
    host.reset();
    external.reset();
    human.reset();
    agent.Stop();
  }
  auto List(const std::string& project = {}) -> Reply {
    return Call(
        *human, A::RequestPayload::ConversationListRequest, [&](Builder& b) {
          return A::CreateConversationListRequest(b, b.CreateString(project))
              .Union();
        });
  }
  auto Search(const std::string& text, const std::string& project = {})
      -> Reply {
    return Call(*human, A::RequestPayload::ConversationSearchRequest,
                [&](Builder& b) {
                  return A::CreateConversationSearchRequest(
                             b, b.CreateString(project), b.CreateString(text))
                      .Union();
                });
  }
  auto Delete(const std::string& id) -> Reply {
    return Call(
        *human, A::RequestPayload::ConversationDeleteRequest, [&](Builder& b) {
          return A::CreateConversationDeleteRequest(b, b.CreateString(id))
              .Union();
        });
  }
};

TEST_F(ConversationStoreFixture,
       ProjectRecordsAndDeletionUseTheProjectDatabase) {
  ModelServer model;
  model.Queue(AnthropicText("HOME_RECORD"));
  model.Queue(AnthropicText("PROJECT_RECORD FIXTURE_API_KEY"));
  Configure(model);
  const auto home = Conversation("home-record");
  ASSERT_TRUE(Send(home, "Keep the Home conversation.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  WaitUntilIdle(home);
  StartProject();
  const auto conversation = Conversation("project-record");
  ASSERT_EQ(Inspect(conversation).Code(), "grant_required");
  Grant(conversation);
  ASSERT_TRUE(Inspect(conversation).Code().empty());
  ASSERT_TRUE(Send(conversation, "PROJECT_RECORD FIXTURE_API_KEY " +
                                     JsonString(project_ready, "/token") + " " +
                                     JsonString(ready, "/user_token") + " " +
                                     JsonString(ready, "/external_token"))
                  .Code()
                  .empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  const auto state = WaitUntilIdle(conversation);
  EXPECT_NE(state.find("[redacted]"), state.npos);
  const auto local = directory.path / "project/.tsugumi/agent.sqlite";
  const auto host_db = directory.path / "state/agent.sqlite";
  for (const auto* table : {"conversations", "turns", "receipts", "events",
                            "model_bindings", "usage", "conversation_resources"}) {
    const auto key =
        std::string(table) == "conversations" ? "id" : "conversation";
    const auto sql = "SELECT count(*) FROM " + std::string(table) + " WHERE " +
                     key + "='project-record'";
    EXPECT_GT(Scalar(local, sql), 0) << table;
    EXPECT_EQ(Scalar(host_db, sql), 0) << table;
  }
  EXPECT_EQ(
      Scalar(local,
             "SELECT count(*) FROM sqlite_master WHERE name IN "
             "('credentials','providers','grants','access_requests','operation_"
             "approvals','projects','folders','skill_catalogs')"),
      0);
  EXPECT_EQ(Scalar(host_db,
                   "SELECT count(*) FROM conversations WHERE "
                   "id='home-record' AND project=''"),
            1);
  EXPECT_EQ(Scalar(local,
                   "SELECT count(*) FROM conversations WHERE id='home-record'"),
            0);
  EXPECT_EQ(Read(local).find("FIXTURE_API_KEY"), std::string::npos);
  EXPECT_EQ(Read(local).find(JsonString(project_ready, "/token")),
            std::string::npos);
  EXPECT_EQ(Read(local).find(JsonString(ready, "/user_token")),
            std::string::npos);
  EXPECT_EQ(Read(local).find(JsonString(ready, "/external_token")),
            std::string::npos);
  EXPECT_EQ(JsonString(Search("PROJECT_RECORD").View(), "/0/session"),
            conversation);
  EXPECT_EQ(Search("HOME_RECORD", project_id).View(), "[]");
  EXPECT_NE(List(project_id).View().find(conversation), std::string::npos);
  ASSERT_TRUE(Delete(conversation).Code().empty());
  EXPECT_EQ(Get(conversation, true).Code(), "not_found");
  EXPECT_EQ(Search("PROJECT_RECORD").View(), "[]");
  EXPECT_EQ(List(project_id).View(), "[]");
  for (const auto* table :
       {"conversations", "turns", "receipts", "events", "model_bindings", "usage", "archives",
        "conversation_resources", "helpers", "pending_sends"})
    EXPECT_EQ(Scalar(local, "SELECT count(*) FROM " + std::string(table)), 0)
        << table;
  EXPECT_EQ(Scalar(host_db, "SELECT count(*) FROM access_prompt_items"), 0);
  EXPECT_NE(Get(home).View().find("HOME_RECORD"), std::string::npos);
  EXPECT_EQ(Conversation("project-record"), conversation);
  EXPECT_EQ(Inspect(conversation).Code(), "grant_required");
}

TEST_F(ConversationStoreFixture,
       CopiedProjectKeepsHistoryButNoHostAuthorityOrSettings) {
  ModelServer model;
  for (int index = 0; index < 3; ++index)
    model.Queue(AnthropicText(
        "PORTABLE_EVIDENCE [Run](run:00000000-0000-4000-8000-000000000001)"));
  model.Queue(AnthropicText("A summary of the earlier work."));
  Configure(model);
  const auto home = Conversation("home-does-not-travel");
  StartProject();
  const auto conversation = Conversation("portable-history");
  Grant(conversation);
  ASSERT_TRUE(Inspect(conversation).Code().empty());
  for (int index = 0; index < 3; ++index) {
    ASSERT_TRUE(Send(conversation, "Keep this work.",
                     "portable-turn-" + std::to_string(index))
                    .Code()
                    .empty());
    ASSERT_TRUE(model.WaitForRequests(index + 1));
    WaitUntilIdle(conversation);
  }
  const auto compacted = Call(
      *human, A::RequestPayload::ConversationActionRequest, [&](Builder& b) {
        return A::CreateConversationActionRequest(
                   b, b.CreateString(conversation), A::Action::Compact)
            .Union();
      });
  ASSERT_TRUE(compacted.Code().empty());
  ASSERT_TRUE(model.WaitForRequests(4));
  WaitUntilIdle(conversation);
  const auto original = Get(conversation, true);
  const auto before = original.View();
  const auto transcript = test::View(
      original.Value()->value_as_ConversationResult()->conversation());
  const auto events = Events(conversation, true).View();
  const auto local = directory.path / "project/.tsugumi/agent.sqlite";
  ASSERT_GT(Scalar(local, "SELECT count(*) FROM archives"), 0);
  StopAgent();
  project.Stop();
  const auto copy = directory.path / "copied-project";
  std::filesystem::copy(directory.path / "project", copy,
                        std::filesystem::copy_options::recursive);
  std::filesystem::remove_all(directory.path / "state");
  StartAgent();
  EXPECT_EQ(List().View(), "[]");
  ASSERT_TRUE(Register(copy, *host).Code().empty());
  EXPECT_EQ(Get(conversation, true).View(), before);
  EXPECT_EQ(Events(conversation, true).View(), events);
  EXPECT_EQ(Get(home, true).Code(), "not_found");
  EXPECT_EQ(Get(conversation).Code(), "permission_denied");
  const auto reopen = Call(
      *external, A::RequestPayload::ConversationCreateRequest, [&](Builder& b) {
        return A::CreateConversationCreateRequest(
                   b, b.CreateString(conversation), b.CreateString(project_id))
            .Union();
      });
  EXPECT_EQ(reopen.Code(), "permission_denied");
  EXPECT_EQ(JsonString(Search("PORTABLE_EVIDENCE").View(), "/0/session"),
            conversation);
  const auto access =
      Call(*human, A::RequestPayload::AccessListRequest, [&](Builder& b) {
        return A::CreateAccessListRequest(b, b.CreateString(conversation))
            .Union();
      });
  ASSERT_TRUE(access.Code().empty());
  const auto* permissions = access.Value()->value_as_AccessList();
  EXPECT_TRUE(!permissions->grants() || permissions->grants()->size() == 0);
  EXPECT_TRUE(!permissions->requests() || permissions->requests()->size() == 0);
  EXPECT_NE(events.find("permissionAnswered"), std::string::npos);
  EXPECT_EQ(Inspect(conversation).Code(), "grant_required");
  Grant(conversation);
  EXPECT_EQ(test::View(Get(conversation)
                           .Value()
                           ->value_as_ConversationResult()
                           ->conversation()),
            transcript);
  EXPECT_EQ(model.Requests().size(), 4u);
  ASSERT_TRUE(Delete(conversation).Code().empty());
  EXPECT_EQ(Scalar(copy / ".tsugumi/agent.sqlite",
                   "SELECT count(*) FROM conversations"),
            0);
  EXPECT_EQ(Scalar(local, "SELECT count(*) FROM conversations"), 1);
}

TEST_F(ConversationStoreFixture,
       HostResetKeepsProjectConversationsAndForgetsHome) {
  const auto home = Conversation("home-reset");
  StartProject();
  const auto conversation = Conversation("project-reset", true);
  StopAgent();
  std::filesystem::remove_all(directory.path / "state");
  StartAgent();
  ASSERT_TRUE(Register(directory.path / "project", *host).Code().empty());
  EXPECT_EQ(JsonString(Get(conversation, true).View(), "/conversation/id"),
            conversation);
  EXPECT_EQ(Get(home, true).Code(), "not_found");
  EXPECT_EQ(JsonString(List(project_id).View(), "/0/id"), conversation);
}

TEST_F(ConversationStoreFixture,
       ASecondHostCannotWriteTheSameProjectUntilTheFirstStops) {
  StartProject();
  const auto conversation = Conversation("locked-project", true);
  const auto duplicate = directory.path / "duplicate-location";
  std::filesystem::create_directories(duplicate);
  EXPECT_EQ(Register(duplicate, *host).Code(), "conflict");
  EXPECT_EQ(JsonString(Get(conversation, true).View(), "/conversation/id"),
            conversation);
  ChildProcess other;
  other.Start(
      {TSUGUMI_AGENT_BINARY, "--state-dir",
       (directory.path / "other-state").string(), "--unix",
       (directory.path / "other.sock").string(), "--listen", "127.0.0.1:0"},
      directory.path / "other.log");
  const auto original_ready = ready;
  ready = Readiness(other, directory.path / "other-state/agent.ready",
                    directory.path / "other.log");
  auto other_human = Connect(true);
  auto other_host = ConnectHost();
  ready = original_ready;
  ASSERT_TRUE(Register(directory.path / "project", *other_host).Code().empty());
  const auto get = [&] {
    return Call(*other_human, A::RequestPayload::ConversationGetRequest,
                [&](Builder& b) {
                  return A::CreateConversationGetRequest(
                             b, b.CreateString(conversation))
                      .Union();
                });
  };
  const auto create = [&] {
    return Call(*other_human, A::RequestPayload::ConversationCreateRequest,
                [&](Builder& b) {
                  return A::CreateConversationCreateRequest(
                             b, b.CreateString("second-host-record"),
                             b.CreateString(project_id))
                      .Union();
                });
  };
  EXPECT_EQ(get().Code(), "busy");
  EXPECT_EQ(create().Code(), "busy");
  EXPECT_EQ(Scalar(directory.path / "project/.tsugumi/agent.sqlite",
                   "SELECT count(*) FROM conversations"),
            1);
  StopAgent();
  EXPECT_EQ(JsonString(get().View(), "/conversation/id"), conversation);
  EXPECT_TRUE(create().Code().empty());
}

TEST_F(ConversationStoreFixture,
       PendingMessagesArePortableRedactedAndNeverAutoResumed) {
  ModelServer model;
  Configure(model);
  StartProject();
  const auto conversation = Conversation("pending-portable");
  const auto sent = Call(
      *external, A::RequestPayload::ConversationSendRequest, [&](Builder& b) {
        return A::CreateConversationSendRequest(
                   b, b.CreateString(conversation),
                   b.CreateString("pending-turn"),
                   b.CreateString("Keep FIXTURE_API_KEY private"),
                   b.CreateVector(std::vector{b.CreateString("selected-card")}))
            .Union();
      });
  ASSERT_TRUE(sent.Code().empty());
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (Events(conversation).View().find("permission") == std::string::npos &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_NE(Events(conversation).View().find("permission"), std::string::npos);
  StopAgent();
  project.Stop();
  const auto local = directory.path / "project/.tsugumi/agent.sqlite";
  EXPECT_EQ(Scalar(local,
                   "SELECT count(*) FROM pending_sends WHERE "
                   "conversation='pending-portable'"),
            1);
  EXPECT_EQ(Scalar(directory.path / "state/agent.sqlite",
                   "SELECT count(*) FROM pending_sends"),
            0);
  EXPECT_EQ(Read(local).find("FIXTURE_API_KEY"), std::string::npos);
  const auto copy = directory.path / "pending-copy";
  std::filesystem::copy(directory.path / "project", copy,
                        std::filesystem::copy_options::recursive);
  std::filesystem::remove_all(directory.path / "state");
  StartAgent();
  ASSERT_TRUE(Register(copy, *host).Code().empty());
  const auto snapshot = Get(conversation, true).View();
  EXPECT_TRUE(JsonBool(snapshot, "/resumable"));
  EXPECT_EQ(JsonString(snapshot, "/interrupted_operation/kind"), "preparing");
  EXPECT_NE(snapshot.find("[redacted]"), std::string::npos);
  EXPECT_TRUE(model.Requests().empty());
  EXPECT_EQ(Scalar(copy / ".tsugumi/agent.sqlite",
                   "SELECT count(*) FROM pending_sends"),
            1);
}

TEST_F(ConversationStoreFixture, AConversationCannotBeReboundToAnotherProject) {
  const auto home = Conversation("fixed-home", true);
  StartProject();
  const auto rebound = Call(
      *human, A::RequestPayload::ConversationCreateRequest, [&](Builder& b) {
        return A::CreateConversationCreateRequest(b, b.CreateString(home),
                                                  b.CreateString(project_id))
            .Union();
      });
  EXPECT_EQ(rebound.Code(), "conflict");
  EXPECT_EQ(JsonString(Get(home, true).View(), "/project_id"), "");
  EXPECT_EQ(List(project_id).View(), "[]");
}

TEST_F(ConversationStoreFixture, ProjectStateSymlinksAreRejected) {
  StartProject();
  const auto outside = directory.path / "outside-agent.sqlite";
  Write(outside, "KEEP_OUTSIDE_STATE");
  std::filesystem::create_symlink(
      outside, directory.path / "project/.tsugumi/agent.sqlite");
  const auto reply = Call(*human, A::RequestPayload::ConversationCreateRequest,
                          [&](Builder& b) {
                            return A::CreateConversationCreateRequest(
                                       b, b.CreateString("linked-state"),
                                       b.CreateString(project_id))
                                .Union();
                          });
  EXPECT_EQ(reply.Code(), "invalid_request");
  EXPECT_EQ(Read(outside), "KEEP_OUTSIDE_STATE");
}

}  // namespace
}  // namespace tsugumi::test
