#include "agent/store.hh"

#include <gtest/gtest.h>
#include <algorithm>

#include "../agent_fixture_server.hh"
#include "agent/wire.hh"
#include "agent/presentation.hh"
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
TEST(AgentPresentation, GroupsRetriesAndKeepsTasksAndApprovalsAtTheirCall) {
  const auto conversation =
      agent::wire::Conversation(R"({"id":"s","archive":[],"messages":[
    {"role":"user","content":[{"type":"text","text":"Copy the pipelines"}]},
    {"role":"assistant","content":[{"type":"tool_use","id":"bad","name":"project_file_read","input":"{\"path\":\"a\"}"}]},
    {"role":"user","content":[{"type":"tool_result","tool_use_id":"bad","is_error":true,"content":"missing"}]},
    {"role":"assistant","content":[{"type":"tool_use","id":"retry","name":"project_file_read","input":"{\"path\":\"a\"}"},{"type":"tool_use","id":"delegate","name":"delegate","input":"{}"}]},
    {"role":"user","content":[{"type":"tool_result","tool_use_id":"retry","is_error":false,"content":"found"}]},
    {"role":"assistant","content":[{"type":"text","text":"Later answer"}]}],
    "tools":[{"id":"approval:retry","name":"operation_approval","state":"completed","result":"Approved","isError":false}],
    "helpers":[{"call_id":"delegate","task":0,"spec":{"name":"Copy pipeline","instruction":"Internal instructions","read_cards":["a","b"],"write_cards":["a","b"],"run_cards":[]},"state":"completed","messages":[{"role":"user","content":[{"type":"text","text":"Private prompt and scope"}]},{"role":"assistant","content":[{"type":"tool_use","id":"h","name":"project_file_read","input":"{}"}]},{"role":"user","content":[{"type":"tool_result","tool_use_id":"h","content":"read"}]},{"role":"assistant","content":[{"type":"text","text":"Copied both"}]}]}]})");
  ASSERT_EQ(conversation.rows.size(), 3u);
  const auto& step = *conversation.rows[1];
  EXPECT_EQ(step.kind, "step");
  EXPECT_EQ(step.recovered, 1u);
  EXPECT_EQ(step.failed, 0u);
  ASSERT_EQ(step.children.size(), 5u);
  EXPECT_EQ(step.children[0]->state, "recovered");
  EXPECT_EQ(step.children[2]->name, "operation_approval");
  const auto& helper = *step.children[4];
  EXPECT_EQ(helper.kind, "helper");
  EXPECT_EQ(helper.name, "Copy pipeline");
  EXPECT_EQ(helper.instruction, "Internal instructions");
  EXPECT_EQ(helper.text, "Copied both");
  EXPECT_NE(helper.scope_en.find("Read: a, b"), std::string::npos);
  EXPECT_NE(helper.scope_ja.find("実行: なし"), std::string::npos);
  ASSERT_EQ(helper.children.size(), 2u);
  EXPECT_EQ(helper.children[0]->kind, "step");
  EXPECT_EQ(conversation.rows[2]->text, "Later answer");
  conversation.helpers.front()->result_json = R"({"error":"Provider unavailable"})";
  const auto failed = agent::Presentation(conversation);
  EXPECT_EQ(failed[1]->failed, 1u);
  EXPECT_EQ(failed[1]->children[4]->state, "failed");

}
TEST(AgentPresentation, SaveRetryMatchesChangedTargetsAcrossNewRevisions) {
  const auto conversation = agent::wire::Conversation(R"({"id":"s","messages":[
    {"role":"assistant","content":[{"type":"tool_use","id":"a","name":"project_save","input":"{\"base_rev\":1,\"changes\":[{\"value_type\":\"WriteFile\",\"value\":{\"path\":\"definitions/a/src/main.py\",\"bytes\":\"bad\"}}]}"}]},
    {"role":"user","content":[{"type":"tool_result","tool_use_id":"a","is_error":true,"content":"save failed"}]},
    {"role":"assistant","content":[{"type":"tool_use","id":"b","name":"project_save","input":"{\"base_rev\":2,\"changes\":[{\"value_type\":\"WriteFile\",\"value\":{\"path\":\"definitions/a/src/main.py\",\"bytes\":\"fixed\"}}]}"}]},
    {"role":"user","content":[{"type":"tool_result","tool_use_id":"b","content":"saved"}]}]})");
  ASSERT_EQ(conversation.rows.size(), 1u);
  EXPECT_EQ(conversation.rows[0]->recovered, 1u);
  EXPECT_EQ(conversation.rows[0]->failed, 0u);
  ASSERT_FALSE(conversation.rows[0]->children[1]->links.empty());
  EXPECT_EQ(conversation.rows[0]->children[1]->links[0]->url, "card:a");
}
TEST(AgentPresentation, DirectSkillTasksStayUnderTheirRequestAndQueuedTasksHaveNames) {
  const auto conversation = agent::wire::Conversation(R"({"id":"s","messages":[
    {"role":"user","display":"$review","content":[{"type":"text","text":"internal"}]},
    {"role":"assistant","content":[{"type":"text","text":"later"}]}],
    "helpers":[{"call_id":"turn","task":0,"anchor":1,"spec":{"name":"review","instruction":"Full instructions","read_cards":["a"],"write_cards":[],"run_cards":[]},"state":"completed","messages":[{"role":"user","content":[{"type":"text","text":"internal prompt"}]},{"role":"assistant","content":[{"type":"text","text":"findings"}]}]}]})");
  ASSERT_EQ(conversation.rows.size(), 3u);
  EXPECT_EQ(conversation.rows[0]->text, "$review");
  EXPECT_EQ(conversation.rows[1]->name, "review");
  EXPECT_EQ(conversation.rows[1]->text, "findings");
  EXPECT_EQ(conversation.rows[2]->text, "later");
  const auto queued = agent::wire::Conversation(R"({"id":"s","messages":[{"role":"assistant","content":[{"type":"tool_use","id":"delegation","name":"delegate","input":"{\"tasks\":[{\"name\":\"Queued review\",\"instruction\":\"Read a\",\"read_cards\":[\"a\"],\"write_cards\":[],\"run_cards\":[]}]}"}]}]})");
  ASSERT_EQ(queued.rows.size(), 1u);
  ASSERT_EQ(queued.rows[0]->children.size(), 2u);
  EXPECT_EQ(queued.rows[0]->children[1]->name, "Queued review");
  EXPECT_EQ(queued.rows[0]->children[1]->state, "queued");
}
TEST(AgentPresentation, AnotherTargetOrNewUserTurnDoesNotHideFailure) {
  const auto conversation = agent::wire::Conversation(R"({"id":"s","messages":[
    {"role":"assistant","content":[{"type":"tool_use","id":"a","name":"project_file_read","input":"{\"path\":\"a\"}"}]},
    {"role":"user","content":[{"type":"tool_result","tool_use_id":"a","is_error":true,"content":"failed a"}]},
    {"role":"assistant","content":[{"type":"tool_use","id":"b","name":"project_file_read","input":"{\"path\":\"b\"}"}]},
    {"role":"user","content":[{"type":"tool_result","tool_use_id":"b","content":"ok b"}]},
    {"role":"user","content":[{"type":"text","text":"New request"}]},
    {"role":"assistant","content":[{"type":"tool_use","id":"c","name":"project_file_read","input":"{\"path\":\"a\"}"}]},
    {"role":"user","content":[{"type":"tool_result","tool_use_id":"c","content":"ok a"}]}]})");
  ASSERT_EQ(conversation.rows.size(), 3u);
  EXPECT_EQ(conversation.rows[0]->failed, 1u);
  EXPECT_EQ(conversation.rows[0]->recovered, 0u);
  EXPECT_EQ(conversation.rows[0]->children[0]->result, "failed a");
}
TEST(AgentPresentation, AReadRetryAfterAnExplanationRecoversTheOriginalStep) {
  const auto conversation = agent::wire::Conversation(R"({"id":"s","messages":[
    {"role":"assistant","content":[{"type":"tool_use","id":"a","name":"project_file_read","input":"{\"path\":\"a\"}"}]},
    {"role":"user","content":[{"type":"tool_result","tool_use_id":"a","is_error":true,"content":"failed"}]},
    {"role":"assistant","content":[{"type":"text","text":"Retrying"},{"type":"tool_use","id":"b","name":"project_file_read","input":"{\"path\":\"a\"}"}]},
    {"role":"user","content":[{"type":"tool_result","tool_use_id":"b","content":"read"}]}]})");
  ASSERT_EQ(conversation.rows.size(), 3u);
  EXPECT_EQ(conversation.rows[0]->failed, 0u);
  EXPECT_EQ(conversation.rows[0]->recovered, 1u);
}
TEST(AgentPresentation, ExternalTasksApprovalsAndReceiptsDoNotRestoreAsAnEmptyPanel) {
  const auto task = agent::wire::Conversation(R"({"id":"s","helpers":[{"call_id":"external","task":0,"anchor":0,"spec":{"name":"External review","instruction":"Review","read_cards":[],"write_cards":[],"run_cards":[]},"messages":[],"state":"running"}],"tools":[{"id":"approval:external","name":"operation_approval","state":"pending","result":"Awaiting approval"}]})");
  ASSERT_EQ(task.rows.size(), 2u);
  EXPECT_EQ(task.rows[0]->name, "External review");
  EXPECT_EQ(task.rows[1]->name, "operation_approval");
  const auto receipt = agent::wire::Conversation(R"({"id":"s","tools":[{"id":"external","name":"core","state":"completed","result":"saved"}]})");
  ASSERT_EQ(receipt.rows.size(), 1u);
  ASSERT_EQ(receipt.rows[0]->children.size(), 1u);
  EXPECT_EQ(receipt.rows[0]->children[0]->result, "saved");
}
TEST(AgentPresentation, ResultsKeepTheirCallIdsAcrossTheArchiveBoundary) {
  const auto conversation = agent::wire::Conversation(R"({"id":"s","archive":[{"role":"assistant","content":[
    {"type":"tool_use","id":"a","name":"project_file_read","input":"{\"path\":\"a\"}"},
    {"type":"tool_use","id":"b","name":"project_file_read","input":"{\"path\":\"b\"}"}]}],
    "messages":[{"role":"user","content":[{"type":"tool_result","tool_use_id":"missing","content":"ignore"},
    {"type":"tool_result","tool_use_id":"a","content":"A rejected","is_error":true},
    {"type":"tool_result","tool_use_id":"b","content":"B read"}]}]})");
  ASSERT_EQ(conversation.rows.size(), 1u);
  EXPECT_TRUE(conversation.rows[0]->archived);
  ASSERT_EQ(conversation.rows[0]->children.size(), 2u);
  EXPECT_EQ(conversation.rows[0]->children[0]->result, "A rejected");
  EXPECT_EQ(conversation.rows[0]->children[0]->state, "failed");
  EXPECT_EQ(conversation.rows[0]->children[1]->result, "B read");
  EXPECT_EQ(conversation.rows[0]->children[1]->state, "completed");
}
TEST(AgentPresentation, LinksUseTypedCardPathsRunReceiptsAndTheRequestedLogInvocation) {
  const auto conversation = agent::wire::Conversation(R"({"id":"s","messages":[
    {"role":"assistant","content":[{"type":"tool_use","id":"run","name":"run","input":"{\"card\":{\"segments\":[\"outer\",\"a\"]}}"}]},
    {"role":"user","content":[{"type":"tool_result","tool_use_id":"run","content":"{\"reply\":\"RunAccepted\",\"value\":{\"run\":{\"run_id\":\"11111111-1111-4111-8111-111111111111\"}}}"}]},
    {"role":"assistant","content":[{"type":"tool_use","id":"log","name":"log_range","input":"{\"run_id\":\"11111111-1111-4111-8111-111111111111\",\"card\":{\"segments\":[\"outer\",\"a\"]},\"invocation\":\"2\",\"stream\":\"standard_error\",\"offset\":\"16\"}"}]}]})");
  ASSERT_EQ(conversation.rows.size(), 1u);
  const auto& calls = conversation.rows[0]->children;
  ASSERT_EQ(calls.size(), 2u);
  std::vector<std::string> run_links, log_links;
  for (const auto& link : calls[0]->links) run_links.push_back(link->url);
  for (const auto& link : calls[1]->links) log_links.push_back(link->url);
  EXPECT_NE(std::find(run_links.begin(), run_links.end(), "card:outer/a"), run_links.end());
  EXPECT_NE(std::find(run_links.begin(), run_links.end(), "run:11111111-1111-4111-8111-111111111111"), run_links.end());
  EXPECT_NE(std::find(log_links.begin(), log_links.end(), "log:11111111-1111-4111-8111-111111111111/outer/a?invocation=2&stream=stderr&offset=16"), log_links.end());
}
TEST(AgentPresentation, InvalidToolArgumentsRemainVisibleInsteadOfBreakingTheSnapshot) {
  const auto conversation = agent::wire::Conversation(R"({"id":"s","messages":[
    {"role":"assistant","content":[{"type":"tool_use","id":"run","name":"run","input":"{\"card\":{\"segments\":null}}"},{"type":"tool_use","id":"save","name":"project_save","input":"{\"changes\":{}}"}]},
    {"role":"user","content":[{"type":"tool_result","tool_use_id":"run","content":"Invalid Card path","is_error":true},{"type":"tool_result","tool_use_id":"save","content":"Invalid changes","is_error":true}]}]})");
  ASSERT_EQ(conversation.rows.size(), 1u);
  EXPECT_EQ(conversation.rows[0]->failed, 2u);
  ASSERT_EQ(conversation.rows[0]->children.size(), 2u);
  EXPECT_EQ(conversation.rows[0]->children[0]->result, "Invalid Card path");
  EXPECT_EQ(conversation.rows[0]->children[1]->result, "Invalid changes");
}
TEST(AgentPresentation, ProviderSearchSourcesStayReadableAndKeepTheirExactUrls) {
  const auto conversation = agent::wire::Conversation(R"({"id":"s","messages":[{"role":"assistant","content":[{"type":"web_search","id":"search","query":"science","sources":[{"title":"Paper","url":"https://example.test/paper"}]}]}]})");
  ASSERT_EQ(conversation.rows.size(), 1u);
  ASSERT_EQ(conversation.rows[0]->children.size(), 1u);
  EXPECT_EQ(conversation.rows[0]->children[0]->result, "Paper — https://example.test/paper");
}
TEST(AgentPresentation, AccessGroupsKeepRawChoicesAndDifferentDurations) {
  const std::vector<std::string> asked{
      R"(project_access {"title":"RCW86","path":"/work/RCW86","access":"Read","scope":"Conversation","needed_now":true})",
      R"(project_access {"title":"RCW86","path":"/work/RCW86","access":"Write","scope":"Conversation","needed_now":false})",
      R"(project_access {"title":"RCW86","path":"/work/RCW86","access":"Run","scope":"Conversation","needed_now":false})"};
  auto groups = agent::AccessGroups(asked);
  ASSERT_EQ(groups.size(), 1u);
  EXPECT_EQ(groups[0]->scope, "Conversation");
  ASSERT_EQ(groups[0]->lines.size(), 3u);
  EXPECT_EQ(groups[0]->lines[0]->request, asked[0]);
  EXPECT_TRUE(groups[0]->lines[0]->needed_now);
  EXPECT_FALSE(groups[0]->lines[1]->needed_now);
  EXPECT_FALSE(groups[0]->lines[2]->needed_now);
  auto mixed = asked;
  mixed.push_back(
      R"(project_access {"title":"RCW86","path":"/work/RCW86","access":"Shell","scope":"Server"})");
  groups = agent::AccessGroups(mixed);
  ASSERT_EQ(groups.size(), 1u);
  EXPECT_TRUE(groups[0]->scope.empty());
  EXPECT_EQ(groups[0]->lines[3]->scope, "Server");
}
TEST_F(ConversationStoreTest,
       LiveProjectionKeepsStreamingTextAcrossCheckpointsAndRestore) {
  std::lock_guard lock(gate.mutex);
  Create("live");
  agent::ContentBlock question;
  question.text = "hello";
  store->SaveMessages(
      "live",
      {{"user", {question}, {}, {}, agent::ProviderAPI::AnthropicMessages}});
  store->RecordEvent("live", R"({"kind":"checkpoint"})");
  store->RecordEvent("live", R"({"kind":"text","text":"First "})");
  store->RecordEvent("live", R"({"kind":"text","text":"answer"})");
  auto snapshot = [&] {
    return agent::wire::Conversation(
        store->ConversationJson(store->GetConversation("live")));
  };
  ASSERT_EQ(snapshot().rows.size(), 2u);
  EXPECT_EQ(snapshot().rows[1]->text, "First answer");
  agent::ContentBlock reply;
  reply.text = "First answer";
  store->SaveMessages(
      "live",
      {{"user", {question}, {}, {}, agent::ProviderAPI::AnthropicMessages},
       {"assistant", {reply}, {}, {}, agent::ProviderAPI::AnthropicMessages}});
  store->RecordEvent("live", R"({"kind":"checkpoint"})");
  store->RecordEvent("live", R"({"kind":"finished"})");
  ASSERT_EQ(snapshot().rows.size(), 2u);
  EXPECT_EQ(snapshot().rows[1]->text, "First answer");
  Open();
  ASSERT_EQ(snapshot().rows.size(), 2u);
  EXPECT_EQ(snapshot().rows[1]->text, "First answer");
}

}  // namespace
}  // namespace tsugumi::test
