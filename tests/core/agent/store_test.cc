#include "agent/store.hh"

#include <gtest/gtest.h>

#include "../agent_fixture_server.hh"
#include "host/failure.hh"
#include "host/store.hh"

namespace tsugumi::test {
namespace {
namespace A = Tsugumi::Agent;
template <class Native, class Call>
auto WithRequest(const Native& request, Call call) -> decltype(auto) {
  using Table = typename Native::TableType;
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(Table::Pack(builder, &request));
  return call(*flatbuffers::GetRoot<Table>(builder.GetBufferPointer()));
}
struct ConversationStoreTest : testing::Test {
  TemporaryDirectory directory;
  host::StateGate gate;
  host::HostStore home{directory.path};
  host::ProjectRegistry registry{
      gate, home.db, [this](const std::string& project) {
        return store ? store->OpenedPath(project) : std::nullopt;
      }};
  std::unique_ptr<agent::ConversationStore> store;
  bool allowed = true;
  bool busy = false;

  ConversationStoreTest() { Open(); }
  auto Open() -> void {
    store = std::make_unique<agent::ConversationStore>(
        gate, home.db, registry, [](std::string text) { return text; },
        [this](const auto&, const auto&, auto) { return allowed; },
        [this](const auto&) { return busy; });
  }
  auto Register(const std::string& project) -> std::filesystem::path {
    const auto root = directory.path / project;
    std::filesystem::create_directories(root);
    Tsugumi::Host::EnvironmentStateT ready;
    ready.project_id = project;
    ready.project = root.string();
    ready.key = project;
    ready.endpoint = "unix:/fixture";
    registry.RegisterReady(ready);
    return root;
  }
  auto Create(const std::string& id, const std::string& project = {},
              bool human = true) -> agent::ConversationStore::Conversation {
    A::ConversationCreateRequestT request;
    request.conversation_id = id;
    request.project_id = project;
    return WithRequest(request, [&](const auto& value) {
      return store->Create(value, human);
    });
  }
};
TEST_F(ConversationStoreTest,
       ProjectDatabasesKeepTheirLocksAndRecoverySeparateFromHome) {
  std::lock_guard lock(gate.mutex);
  const auto root = Register("project");
  Create("home");
  Create("local", "project");
  EXPECT_EQ(store->OpenedPath("project"),
            std::filesystem::canonical(root) / ".tsugumi");
  auto home_count = home.db.prepare("SELECT count(*) FROM conversations");
  ASSERT_TRUE(home_count.step());
  EXPECT_EQ(home_count.i64(0), 1);
  auto& project = store->ConversationDatabase("local");
  EXPECT_NE(&project, &home.db);
  {
    auto journal = project.prepare("PRAGMA journal_mode");
    ASSERT_TRUE(journal.step());
    EXPECT_EQ(journal.text(0), "delete");
  }
  EXPECT_FALSE(fs::FileLock::try_acquire(root / ".tsugumi/agent.lock"));
  for (auto* db : {&home.db, &project})
    db->exec(
        "INSERT INTO turns VALUES('fixture','turn','digest','running',0,'{}')");
  store.reset();
  {
    auto released = fs::FileLock::try_acquire(root / ".tsugumi/agent.lock");
    ASSERT_TRUE(released);
  }
  Open();
  for (auto* db : {&home.db, &store->ConversationDatabase("local")}) {
    auto turn =
        db->prepare("SELECT state FROM turns WHERE conversation='fixture'");
    ASSERT_TRUE(turn.step());
    EXPECT_EQ(turn.text(0), "interrupted");
  }
}
TEST_F(ConversationStoreTest,
       ArchivesSearchResourcesAndEventsKeepHistoryAuthorization) {
  std::lock_guard lock(gate.mutex);
  Register("project");
  Create("external", "project", false);
  Create("owned", "project", true);
  agent::ContentBlock content;
  content.text = "Archived needle";
  content.resource_ids = {"project"};
  store->SaveMessages(
      "external",
      {{"user", {content}, {}, {}, agent::ProviderAPI::AnthropicMessages}});
  store->SaveMessages("external", {});
  EXPECT_EQ(store->Resources("external"), std::vector<std::string>{"project"});
  A::ConversationSearchRequestT search;
  search.text = "needle";
  EXPECT_EQ(WithRequest(
                search,
                [&](const auto& value) { return store->Search(value, false); })
                .matches.size(),
            1);
  store->RecordEvent("external", R"({"kind":"finished"})");
  store->RecordEvent("external", R"({"kind":"finished"})");
  A::EventsRequestT events;
  events.conversation_id = "external";
  events.limit = 0;
  busy = true;
  const auto page = WithRequest(
      events, [&](const auto& value) { return store->Events(value, false); });
  EXPECT_EQ(page.events.size(), 1);
  EXPECT_TRUE(page.busy);
  EXPECT_GT(page.next_seq, 0);
  A::ConversationListRequestT list;
  EXPECT_EQ(
      WithRequest(list,
                  [&](const auto& value) { return store->List(value, false); })
          .conversations.size(),
      1);
  allowed = false;
  EXPECT_THROW(store->AuthorizeHistory("external"), host::Failure);
  EXPECT_TRUE(WithRequest(search, [&](const auto& value) {
                return store->Search(value, false);
              }).matches.empty());
  EXPECT_TRUE(WithRequest(list, [&](const auto& value) {
                return store->List(value, false);
              }).conversations.empty());
  EXPECT_THROW(WithRequest(events,
                           [&](const auto& value) {
                             return store->Events(value, false);
                           }),
               host::Failure);
  EXPECT_THROW(store->OwnConversation("owned", false), host::Failure);
}
TEST_F(ConversationStoreTest,
       EqualCountAndGrowingReplacementsArchiveEveryOriginal) {
  Create("conversation");
  agent::ContentBlock first;
  first.text = "original request";
  agent::ContentBlock result;
  result.type = "tool_result";
  result.id = "call";
  result.text = "FULL_RESULT_BEFORE_SHORTENING";
  std::vector<agent::Message> messages{
      {"user", {first}, {}, {}, agent::ProviderAPI::AnthropicMessages},
      {"user", {result}, {}, {}, agent::ProviderAPI::AnthropicMessages}};
  store->SaveMessages("conversation", messages);
  messages.back().content[0].text = "shortened";
  store->SaveMessages("conversation", messages);  // Same message count.
  auto summary = messages.front();
  summary.content[0].text = "summary";
  summary.context_summary = true;
  messages.front() = summary;
  first.text = "next request";
  messages.push_back(
      {"user", {first}, {}, {}, agent::ProviderAPI::AnthropicMessages});
  store->SaveMessages("conversation", messages);  // Count increased.
  const auto human =
      store->ConversationJson(store->GetConversation("conversation"));
  EXPECT_NE(human.find("FULL_RESULT_BEFORE_SHORTENING"), std::string::npos);
  EXPECT_NE(human.find("original request"), std::string::npos);
  auto archives =
      home.db.prepare("SELECT messages FROM archives ORDER BY rowid");
  ASSERT_TRUE(archives.step());
  EXPECT_EQ(agent::ParseMessages(archives.text(0)).at(0).content[0].text,
            "FULL_RESULT_BEFORE_SHORTENING");
  ASSERT_TRUE(archives.step());
  EXPECT_EQ(agent::ParseMessages(archives.text(0)).at(0).content[0].text,
            "original request");
  EXPECT_FALSE(archives.step());
}

TEST_F(ConversationStoreTest,
       RepeatedAssistantAnswersKeepTheExactVisibleOrder) {
  Create("repeated");
  std::vector<agent::Message> original;
  for (int i = 0; i < 4; ++i) {
    agent::ContentBlock user;
    user.text = "hello " + std::to_string(i);
    agent::ContentBlock answer;
    answer.text = "the same assistant answer";
    original.push_back(
        {"user", {user}, {}, {}, agent::ProviderAPI::AnthropicMessages});
    original.push_back(
        {"assistant", {answer}, {}, {}, agent::ProviderAPI::AnthropicMessages});
  }
  store->SaveMessages("repeated", original);
  agent::ContentBlock summary;
  summary.text = "internal handoff";
  std::vector<agent::Message> compacted{
      {"user", {summary}, {}, {}, agent::ProviderAPI::AnthropicMessages}};
  compacted.front().context_summary = true;
  compacted.insert(compacted.end(), original.begin() + 4, original.end());
  store->SaveMessages("repeated", compacted);
  std::vector<agent::Message> visible;
  auto archived = home.db.prepare(
      "SELECT messages FROM archives WHERE conversation='repeated' ORDER BY "
      "rowid");
  while (archived.step()) {
    const auto messages = agent::ParseMessages(archived.text(0));
    visible.insert(visible.end(), messages.begin(), messages.end());
  }
  for (const auto& message :
       agent::ParseMessages(store->GetConversation("repeated").messages))
    if (!message.context_summary) visible.push_back(message);
  EXPECT_EQ(agent::SerializeMessages(visible),
            agent::SerializeMessages(original));
  EXPECT_EQ(visible.size(), 8u);
}

TEST_F(ConversationStoreTest, AppendingToolBlocksDoesNotDuplicateHistory) {
  Create("conversation");
  agent::ContentBlock block;
  block.type = "tool_result";
  block.id = "one";
  block.text = "first result";
  std::vector<agent::Message> messages{
      {"user", {block}, {}, {}, agent::ProviderAPI::AnthropicMessages}};
  store->SaveMessages("conversation", messages);
  block.id = "two";
  messages.back().content.push_back(block);
  store->SaveMessages("conversation", messages);
  store->SaveMessages("conversation", messages);
  auto archives = home.db.prepare("SELECT count(*) FROM archives");
  ASSERT_TRUE(archives.step());
  EXPECT_EQ(archives.i64(0), 0);
  // A failed replacement cannot erase the original or leave a partial archive.
  home.db.exec(
      "CREATE TRIGGER fail_save BEFORE UPDATE ON conversations BEGIN "
      "SELECT RAISE(ABORT,'fixture'); END");
  EXPECT_THROW(store->SaveMessages("conversation", {}), std::exception);
  EXPECT_EQ(
      agent::ParseMessages(store->GetConversation("conversation").messages)
          .size(),
      1u);
  auto count = home.db.prepare("SELECT count(*) FROM archives");
  ASSERT_TRUE(count.step());
  EXPECT_EQ(count.i64(0), 0);
}

TEST_F(ConversationStoreTest,
       KnownConversationsRemainReadableWhileAnotherProjectIsLocked) {
  std::lock_guard lock(gate.mutex);
  Register("known");
  const auto other = Register("other");
  Create("conversation", "known");
  Create("unrelated", "other");
  store.reset();
  auto other_lock = fs::FileLock::try_acquire(other / ".tsugumi/agent.lock");
  ASSERT_TRUE(other_lock);
  Open();
  ASSERT_NE(store->ProjectDatabase("known"), nullptr);
  EXPECT_EQ(store->GetConversation("conversation").project, "known");
  EXPECT_THROW(store->Databases(), host::Failure);
}
TEST_F(ConversationStoreTest,
       ProjectStateLinksAndForeignConversationIdentitiesStayRejected) {
  std::lock_guard lock(gate.mutex);
  const auto root = Register("project");
  const auto outside = directory.path / "outside";
  std::filesystem::create_directories(outside);
  std::filesystem::create_directory_symlink(outside, root / ".tsugumi");
  EXPECT_THROW(store->ProjectDatabase("project", true), host::Failure);
  std::filesystem::remove(root / ".tsugumi");
  Create("conversation", "project");
  store.reset();
  home.db.exec("INSERT INTO project_paths VALUES('foreign','" + root.string() +
               "')");
  Open();
  try {
    store->ProjectDatabase("foreign");
    FAIL() << "A foreign project's conversation database was accepted";
  } catch (const host::Failure& error) {
    EXPECT_EQ(error.code, "conflict");
  }
}
TEST_F(ConversationStoreTest,
       DeletionCommitsHostRevocationBeforeProjectDeletion) {
  std::lock_guard lock(gate.mutex);
  Register("project");
  Create("conversation", "project");
  home.db.exec(
      "INSERT INTO grants VALUES('grant','project','conversation',0,1);"
      "INSERT INTO providers VALUES('conversation','{}')");
  store->RecordEvent("conversation", R"({"kind":"finished"})");
  auto& project = store->ConversationDatabase("conversation");
  project.exec(
      "CREATE TRIGGER reject_delete BEFORE DELETE ON conversations "
      "BEGIN SELECT RAISE(ABORT,'fixture'); END");
  EXPECT_THROW(store->Delete("conversation"), sql::Error);
  auto grant =
      home.db.prepare("SELECT 1 FROM grants WHERE conversation='conversation'");
  EXPECT_FALSE(grant.step());
  auto provider = home.db.prepare(
      "SELECT 1 FROM providers WHERE conversation='conversation'");
  EXPECT_FALSE(provider.step());
  EXPECT_EQ(store->GetConversation("conversation").project, "project");
  auto event = project.prepare(
      "SELECT count(*) FROM events WHERE conversation='conversation'");
  ASSERT_TRUE(event.step());
  EXPECT_EQ(event.i64(0), 1);
  // Finish the read before writing the same connection in the next deletion.
  EXPECT_FALSE(event.step());
  project.exec("DROP TRIGGER reject_delete");
  busy = true;
  EXPECT_THROW(store->Delete("conversation"), host::Failure);
  busy = false;
  EXPECT_NO_THROW(store->Delete("conversation"));
  EXPECT_THROW(store->GetConversation("conversation"), host::Failure);
}
}  // namespace
}  // namespace tsugumi::test
