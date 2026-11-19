#include "project_fixture.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::test {
namespace {
namespace json = ::tsugumi::json;
TEST_F(AgentProjectFixture,
       AgentSavePolicyIsAppliedByTheHostAndItsReceiptSurvivesRestart) {
  Write(directory.path / "project/definitions/plan/_card.json",
        R"({"template":"draft"})");
  Write(
      directory.path / "project/definitions/plan/_meta.json",
      R"({"description":"Plan","content":"Keep these words","comments":[{"id":"c1","text":"Ask","author":"user"}]})");
  StartProject(
      false,
      R"({"name":"PRIVATE_PROJECT_SENTINEL","agentExclude":[],"pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})");
  const auto conversation = Conversation("save-policy");
  Grant(conversation);
  Grant(conversation, Tsugumi::Service::Access::Write);
  auto inspect = [&] {
    return json::Object(json::Get(
        json::Object(JsonString(Inspect(conversation).View(), "/text")),
        "value"));
  };
  const auto snapshot = inspect();
  const auto rev = json::Get(snapshot, "rev");
  auto save = [&](const json::Members& value) {
    return json::Encode(json::Members{
        {"base_rev", rev},
        {"changes",
         json::Encode(std::vector<std::string>{json::Encode(
             json::Members{{"value_type", json::Quote("PutDefinition")},
                           {"value", json::Encode(value)}})})}});
  };
  json::Members value{{"id", json::Quote("plan")},
                      {"card_json", json::Quote(R"({"template":"sink"})")},
                      {"keep_meta", "true"}};
  const auto refused =
      Tool(conversation, "project_save", save(value), "refused");
  ASSERT_TRUE(refused.Code().empty()) << refused.Code();
  EXPECT_TRUE(JsonBool(refused.View(), "/isError"));
  EXPECT_EQ(JsonString(JsonString(refused.View(), "/text"), "/value/kind"),
            "validation_failed");
  // A policy refusal is a terminal host receipt, not an interrupted core save.
  EXPECT_EQ(Tool(conversation, "project_save", save(value), "refused").View(),
            refused.View());
  value["meta_json"] = json::Quote(
      R"({"description":"Plan","content":"Keep these words","context":"For the report","comments":[{"id":"c1","text":"Ask","author":"agent","resolved":true,"stateBy":"user","resolution":"Done"},{"text":"New","author":"user"}]})");
  const auto args = save(value);
  const auto saved = Tool(conversation, "project_save", args, "saved");
  ASSERT_TRUE(saved.Code().empty());
  EXPECT_FALSE(JsonBool(saved.View(), "/isError"));
  const auto body = JsonString(saved.View(), "/text");
  EXPECT_EQ(JsonString(body, "/value/comments/0/card"), "plan");
  EXPECT_EQ(JsonString(body, "/value/comments/0/by"), "agent");
  auto meta = json::Object(
      Read(directory.path / "project/definitions/plan/_meta.json"));
  auto comments = json::Array(json::Get(meta, "comments"));
  EXPECT_EQ(json::Text(json::Object(comments[0]), "stateBy"), "agent");
  EXPECT_EQ(Tool(conversation, "project_save", args, "saved").View(),
            saved.View());
  human.reset();
  external.reset();
  host.reset();
  service.Stop();
  StartHost();
  EXPECT_EQ(Tool(conversation, "project_save", args, "saved").View(),
            saved.View());
  const auto initialized = ApprovedTool(conversation, "version_init", "{}");
  ASSERT_TRUE(initialized.Code().empty());
  const auto commit =
      JsonString(JsonString(initialized.View(), "/text"), "/value/commit");
  const auto graph = Tool(conversation, "version_graph", "{}");
  ASSERT_TRUE(graph.Code().empty());
  EXPECT_EQ(JsonString(JsonString(graph.View(), "/text"),
                       "/value/commits/0/comments/0/card"),
            "plan");
  const auto record =
      Call(*host, H::RequestPayload::VersionCommentsRequest, [&](Builder& b) {
        return H::CreateVersionCommentsRequest(b, b.CreateString(project_id),
                                               b.CreateString(commit))
            .Union();
      });
  ASSERT_TRUE(record.Code().empty()) << record.Code();
  const auto* list = record.HostValue()->value_as_VersionComments()->comments();
  ASSERT_NE(list, nullptr);
  EXPECT_EQ(list->size(), 2u);
}
}  // namespace
}  // namespace tsugumi::test

namespace tsugumi::test {
namespace {
TEST_F(AgentProjectFixture, InspectionPreservesCoreFailureDiagnostics) {
  const auto nested = directory.path / "project/externals/storage";
  Write(nested / "tsugumi.json",
        R"({"name":"Nested","canvas":{"cards":[],"connections":[]}})");
  const auto definition = nested / "definitions/child/_card.json";
  Write(definition, R"({"template":"draft"})");
  StartProject();
  const auto conversation = Conversation("inspect-diagnostic");
  Grant(conversation);
  ASSERT_TRUE(Inspect(conversation).Code().empty());
  // The confirmed snapshot still has the old hash. Reading its external
  // definition must fail with the core's diagnostic, not an empty message.
  Write(definition, R"({"template":"draft","network":true})");
  const auto refused = Inspect(conversation);
  ASSERT_EQ(refused.Code(), "external_change");
  ASSERT_NE(refused.Failure(), nullptr);
  EXPECT_NE(refused.Failure()->message()->str().find(
                "externals/storage/definitions/child/_card.json"),
            std::string::npos);
}

TEST_F(AgentProjectFixture, StaleWholeDocumentSaveRefusesAndKeepsInput) {
  StartProject();
  const auto conversation = Conversation("stale-save");
  Grant(conversation);
  Grant(conversation, Tsugumi::Service::Access::Write);
  const auto snapshot = json::Object(
      json::Get(json::Object(JsonString(Inspect(conversation).View(), "/text")),
                "value"));
  const auto edited = Tool(
      conversation, "project_edit",
      R"({"edits":[{"file_path":"tsugumi.json","selector":[{"field":"name"}],"old_value":{"present":true,"value":"PRIVATE_PROJECT_SENTINEL"},"new_value":{"present":true,"value":"Current"}}]})",
      "edit");
  ASSERT_FALSE(JsonBool(edited.View(), "/isError")) << edited.View();
  const auto before = Read(directory.path / "project/tsugumi.json");
  auto proposal = json::Object(json::Text(snapshot, "project_json"));
  proposal["name"] = json::Quote("Keep my input");
  const auto input = json::Encode(json::Members{
      {"base_rev", json::Get(snapshot, "rev")},
      {"changes",
       json::Encode(std::vector<std::string>{json::Encode(json::Members{
           {"value_type", json::Quote("SetProject")},
           {"value",
            json::Encode(json::Members{
                {"json", json::Quote(json::Encode(proposal))}})}})})}});
  const auto refused = Tool(conversation, "project_save", input, "stale");
  ASSERT_TRUE(refused.Code().empty()) << refused.View();
  EXPECT_TRUE(JsonBool(refused.View(), "/isError"));
  EXPECT_EQ(JsonString(JsonString(refused.View(), "/text"), "/value/kind"),
            "revision_conflict");
  const auto replay = Tool(conversation, "project_save", input, "stale");
  ASSERT_TRUE(replay.Code().empty()) << replay.View();
  EXPECT_TRUE(JsonBool(replay.View(), "/isError"));
  EXPECT_EQ(JsonString(JsonString(replay.View(), "/text"), "/value/kind"),
            "revision_conflict");
  EXPECT_EQ(Read(directory.path / "project/tsugumi.json"), before);
}

TEST_F(AgentProjectFixture,
       HelperRechecksAliasesAddedWhilePermissionIsPending) {
  Write(directory.path / "project/definitions/outer/_link.json",
        R"({"external":"storage"})");
  Write(directory.path / "project/definitions/writer/_card.json",
        R"({"template":"draft"})");
  Write(directory.path / "project/definitions/alias/_card.json",
        R"({"template":"draft"})");
  Write(
      directory.path / "project/externals/storage/tsugumi.json",
      R"({"name":"Nested","canvas":{"cards":[{"id":"child"}],"connections":[]}})");
  Write(
      directory.path / "project/externals/storage/definitions/child/_card.json",
      R"({"template":"draft"})");
  const auto source = directory.path /
                      "project/externals/storage/definitions/child/src/main.py";
  Write(source, "original\n");
  StartProject();
  const auto conversation = Conversation("pending-alias");
  Grant(conversation);
  Grant(conversation, Tsugumi::Service::Access::Write);
  auto inspect = [&] {
    return json::Object(json::Get(
        json::Object(JsonString(Inspect(conversation).View(), "/text")),
        "value"));
  };
  auto snapshot = inspect();
  auto project_doc = json::Object(json::Text(snapshot, "project_json"));
  project_doc["canvas"] =
      R"({"cards":[{"id":"outer"},{"id":"alias"},{"id":"writer"}],"connections":[]})";
  const auto seeded = Tool(
      conversation, "project_save",
      json::Encode(json::Members{
          {"base_rev", json::Get(snapshot, "rev")},
          {"changes",
           json::Encode(std::vector<std::string>{json::Encode(json::Members{
               {"value_type", json::Quote("SetProject")},
               {"value",
                json::Encode(json::Members{
                    {"json", json::Quote(json::Encode(project_doc))}})}})})}}),
      "seed");
  ASSERT_FALSE(JsonBool(seeded.View(), "/isError")) << seeded.View();
  snapshot = inspect();
  const auto file =
      Tool(conversation, "project_file_stat",
           json::Encode(json::Members{
               {"path",
                json::Quote("externals/storage/definitions/child/src/main.py")},
               {"at_rev", json::Get(snapshot, "rev")}}));
  ASSERT_TRUE(file.Code().empty()) << file.Code();
  ASSERT_FALSE(JsonBool(file.View(), "/isError")) << file.View();
  const auto hash = JsonString(JsonString(file.View(), "/text"), "/value/hash");
  ASSERT_FALSE(hash.empty()) << file.View();
  ModelServer model;
  Configure(model);
  model.Queue(AnthropicTool(
      "save", "project_edit",
      json::Encode(json::Members{
          {"edits",
           json::Encode(std::vector<std::string>{
               json::Encode(json::Members{
                   {"file_path",
                    json::Quote(
                        "externals/storage/definitions/child/src/main.py")},
                   {"put", json::Quote("changed\n")},
                   {"expected_hash", json::Quote(hash)}}),
               R"({"file_path":"definitions/writer/_card.json","selector":[{"field":"network"}],"old_value":{"present":false},"new_value":{"present":true,"value":true}})"})}})));
  model.Queue(AnthropicText("Finished."));
  auto delegated = std::async(std::launch::async, [&] {
    return Delegate(
        conversation,
        R"({"tasks":[{"name":"scoped","instruction":"Save the supplied edits.","read_cards":["outer/child","writer"],"write_cards":["outer/child","writer"],"run_cards":[]}]})",
        "delegate");
  });
  // Always unblock approval on an assertion failure before joining the helper.
  std::string approval;
  // Agent save approvals are read through AccessListRequest without a push event.
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (approval.empty() && std::chrono::steady_clock::now() < deadline) {
    const auto list =
        Call(*human, A::RequestPayload::AccessListRequest,
             [&](Builder& b) { return A::CreateAccessListRequest(b).Union(); });
    if (const auto* requests = list.Value()->value_as_AccessList()->requests())
      for (const auto* item : *requests)
        if (item->access() == Tsugumi::Service::Access::CardPermissions)
          approval = item->id()->str();
    if (approval.empty()) std::this_thread::sleep_for(10ms);
  }
  const std::shared_ptr<void> unblock(&model, [&](void*) {
    if (approval.empty()) {
      service.Stop();
      return;
    }
    (void)Call(*human, A::RequestPayload::AccessResolveRequest,
               [&](Builder& b) {
                 return A::CreateAccessResolveRequest(
                            b, b.CreateString(approval), false)
                     .Union();
               });
  });
  ASSERT_FALSE(approval.empty());
  // The parent links an out-of-scope container to the same external while the
  // helper waits. Re-applying the operation must recheck its complete scope.
  const auto alias = Tool(
      conversation, "project_save",
      json::Encode(json::Members{
          {"base_rev", json::Get(snapshot, "rev")},
          {"changes",
           json::Encode(std::vector<std::string>{
               R"({"value_type":"PutDefinition","value":{"id":"alias","card_json":"","link_json":"{\"external\":\"storage\"}","meta_json":"{\"description\":\"Shared container\"}"}})"})}}),
      "add-alias");
  EXPECT_FALSE(JsonBool(alias.View(), "/isError")) << alias.View();
  ASSERT_TRUE(Call(*human, A::RequestPayload::AccessResolveRequest,
                   [&](Builder& b) {
                     return A::CreateAccessResolveRequest(
                                b, b.CreateString(approval), true)
                         .Union();
                   })
                  .Code()
                  .empty());
  const auto result = delegated.get();
  ASSERT_TRUE(result.Code().empty()) << result.Code();
  ASSERT_TRUE(model.WaitForRequests(2));
  const auto requests = model.Requests();
  EXPECT_TRUE(JsonBool(requests[1], "/messages/2/content/0/is_error"));
  // The helper receives a redacted failure; its parent can inspect the full
  // durable receipt. Assert the actual scope refusal, not just any error.
  const auto history = Get(conversation);
  ASSERT_TRUE(history.Code().empty()) << history.Code();
  const auto* receipts =
      history.Value()->value_as_ConversationResult()->conversation()->tools();
  ASSERT_NE(receipts, nullptr);
  bool scope_denied = false;
  for (const auto* receipt : *receipts) {
    if (!receipt->is_error() || !receipt->result_json()) continue;
    const auto raw = receipt->result_json()->str();
    if (!raw.starts_with('{')) continue;
    const auto envelope = json::Object(raw);
    if (json::Text(envelope, "reply") != "Failure") continue;
    const auto failure = json::Object(json::Get(envelope, "value", "{}"));
    if (json::Text(failure, "kind") == "permission_denied") scope_denied = true;
  }
  EXPECT_TRUE(scope_denied) << history.View();
  EXPECT_EQ(Read(source), "original\n");
  EXPECT_EQ(Read(directory.path / "project/definitions/writer/_card.json"),
            R"({"template":"draft"})");
}
}  // namespace
}  // namespace tsugumi::test

namespace tsugumi::test {
namespace {
TEST_F(AgentProjectFixture, NestedScopedCreatorGetsAPlaceInItsAddingRevision) {
  namespace json = ::tsugumi::json;
  const auto nested = directory.path / "project/externals/storage";
  const std::string document =
      R"({"name":"Nested","pkgsPath":"tsugumi:default","boundary":{"inputs":{},"outputs":{}},"canvas":{"cards":[{"id":"upstream"},{"id":"secret"}],"connections":[]}})";
  Write(nested / "tsugumi.json", document);
  Write(nested / "definitions/upstream/_card.json", R"({"template":"draft"})");
  Write(nested / "definitions/secret/_card.json", R"({"template":"draft"})");
  StartProject();
  const auto conversation = Conversation("nested-creator");
  Grant(conversation);
  Grant(conversation, Tsugumi::Service::Access::Write);
  const auto initial = json::Object(
      json::Get(json::Object(JsonString(Inspect(conversation).View(), "/text")),
                "value"));
  const auto root =
      R"({"name":"PRIVATE_PROJECT_SENTINEL","pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"outer"}],"connections":[]}})";
  const auto seeded = Tool(
      conversation, "project_save",
      json::Encode(json::Members{
          {"base_rev", json::Get(initial, "rev")},
          {"changes",
           json::Encode(std::vector<std::string>{
               json::Encode(json::Members{
                   {"value_type", json::Quote("SetProject")},
                   {"value",
                    json::Encode(json::Members{{"json", json::Quote(root)}})}}),
               R"({"value_type":"PutDefinition","value":{"id":"outer","card_json":"","link_json":"{\"external\":\"storage\"}"}})"})}}),
      "seed");
  ASSERT_FALSE(JsonBool(seeded.View(), "/isError")) << seeded.View();
  const auto snapshot = json::Object(
      json::Get(json::Object(JsonString(Inspect(conversation).View(), "/text")),
                "value"));
  const auto rev = json::Get(snapshot, "rev");
  const auto before_layout = json::Object(Read(nested / "layout.json"));
  ModelServer model;
  Configure(model);
  model.Queue(AnthropicTool(
      "reserve", "card_id",
      R"({"name":"Nested helper","project_path":"externals/storage/"})"));
  auto delegated = std::async(std::launch::async, [&] {
    return Tool(
        conversation, "delegate",
        R"({"tasks":[{"name":"nested","instruction":"Add a Card wired to upstream.","read_cards":["outer/upstream"],"write_cards":["outer/upstream"],"run_cards":[]}]})",
        "nested-delegate");
  });
  const std::shared_ptr<void> finish(
      &model, [&](void*) { model.Queue(AnthropicText("End.")); });
  ASSERT_TRUE(model.WaitForRequests(2));
  const auto reserved =
      JsonString(model.Requests()[1], "/messages/2/content/0/content");
  ASSERT_TRUE(reserved.starts_with('{')) << reserved;
  const auto id = JsonString(reserved, "/value/id");
  ASSERT_FALSE(id.empty());
  const auto added = json::Encode(json::Members{{"id", json::Quote(id)}});
  const auto wire = json::Encode(json::Members{
      {"from", R"({"card":"upstream","port":""})"},
      {"to", json::Encode(json::Members{{"card", json::Quote(id)},
                                        {"port", json::Quote("")}})}});
  const auto edit = [&](const std::string& member, const std::string& value) {
    return json::Encode(json::Members{
        {"file_path", json::Quote("externals/storage/tsugumi.json")},
        {"selector",
         json::Encode(std::vector<std::string>{
             R"({"field":"canvas"})",
             json::Encode(json::Members{{"field", json::Quote(member)}}),
             json::Encode(json::Members{{"match", value}})})},
        {"old_value", R"({"present":false})"},
        {"new_value",
         json::Encode(json::Members{{"present", "true"}, {"value", value}})}});
  };
  const auto args = json::Encode(json::Members{
      {"edits",
       json::Encode(std::vector<std::string>{
           edit("cards", added), edit("connections", wire),
           json::Encode(json::Members{
               {"file_path", json::Quote("externals/storage/definitions/" + id +
                                         "/_card.json")},
               {"put", json::Quote(R"({"template":"draft"})")},
               {"expect_missing", "true"}})})}});
  model.Queue(AnthropicTool("save", "project_edit", args));
  model.Queue(AnthropicText("Saved."));
  ASSERT_TRUE(delegated.get().Code().empty());
  ASSERT_TRUE(model.WaitForRequests(3));
  const auto response =
      JsonString(model.Requests()[2], "/messages/4/content/0/content");
  EXPECT_FALSE(JsonBool(model.Requests()[2], "/messages/4/content/0/is_error"))
      << response;
  const auto final = json::Object(
      json::Get(json::Object(JsonString(Inspect(conversation).View(), "/text")),
                "value"));
  EXPECT_EQ(json::UInt(json::Text(final, "rev")),
            json::UInt(json::String(rev, rev)) + 1);
  const auto positions =
      json::Object(json::Object(Read(nested / "layout.json")).at("cards"));
  EXPECT_TRUE(positions.contains(id));
  for (const auto& [key, value] : json::Object(before_layout.at("cards")))
    EXPECT_EQ(positions.at(key), value);
  EXPECT_NE(Read(nested / "tsugumi.json").find("secret"), std::string::npos);
  EXPECT_EQ(response.find("secret"), std::string::npos);
}
}  // namespace
}  // namespace tsugumi::test

namespace tsugumi::test {
namespace {
class ScopedSourceOperation : public AgentProjectFixture,
                              public ::testing::WithParamInterface<bool> {};
TEST_P(ScopedSourceOperation, SourceEditsMaterializeOnlyWritableCards) {
  const auto root = directory.path / "project";
  const std::string graph =
      R"({"name":"Nested","pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"a"},{"id":"b"}],"connections":[]}})";
  for (const auto& prefix : {std::string{}, std::string{"externals/assigned/"},
                             std::string{"externals/unrelated/"}}) {
    for (const auto* id : {"a", "b"})
      Write(root / prefix / "definitions" / id / "_card.json",
            R"({"template":"draft"})");
    if (!prefix.empty()) {
      Write(root / prefix / "tsugumi.json", graph);
      Write(root / prefix / "layout.json", "{}");
    }
  }
  Write(root / "definitions/outer/_link.json", R"({"external":"assigned"})");
  StartProject(
      false,
      R"({"name":"Scoped","pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"a"},{"id":"b"},{"id":"outer"}],"connections":[]}})");
  const auto conversation = Conversation("source-only-scoped");
  Grant(conversation);
  // A read grant never authorizes a save or its implicit placement writes.
  const auto tool = GetParam() ? "project_edit" : "project_save";
  const auto snapshot = json::Object(
      json::Get(json::Object(JsonString(Inspect(conversation).View(), "/text")),
                "value"));
  const std::string args =
      GetParam()
          ? R"json({"edits":[{"file_path":"definitions/a/code.py","put":"print(1)","expect_missing":true},{"file_path":"externals/assigned/definitions/a/code.py","put":"print(2)","expect_missing":true}]})json"
          : json::Encode(json::Members{
                {"base_rev", json::Get(snapshot, "rev")},
                {"changes",
                 R"json([{"value_type":"PutSource","value":{"path":"definitions/a/code.py","bytes":"print(1)"}},{"value_type":"PutSource","value":{"path":"externals/assigned/definitions/a/code.py","bytes":"print(2)"}}])json"}});
  const auto refused = Tool(conversation, tool, args, "no-write");
  EXPECT_EQ(refused.Code(), "grant_required");
  EXPECT_EQ(Read(root / "layout.json"), "{}");
  Grant(conversation, Tsugumi::Service::Access::Write);
  ModelServer model;
  Configure(model);
  model.Queue(AnthropicTool("save", tool, args));
  model.Queue(AnthropicText("Saved."));
  const auto delegated = Delegate(
      conversation,
      R"({"tasks":[{"name":"source","instruction":"Save the supplied source edits.","read_cards":["a","outer/a"],"write_cards":["a","outer/a"],"run_cards":[]}]})",
      "source-delegate");
  ASSERT_TRUE(delegated.Code().empty()) << delegated.Code();
  ASSERT_TRUE(model.WaitForRequests(2));
  EXPECT_FALSE(JsonBool(model.Requests()[1], "/messages/2/content/0/is_error"))
      << model.Requests()[1];
  for (const auto& prefix :
       {std::string{}, std::string{"externals/assigned/"}}) {
    EXPECT_EQ(Read(root / prefix / "definitions/a/code.py"),
              prefix.empty() ? "print(1)" : "print(2)");
    const auto places = json::Object(
        json::Get(json::Object(Read(root / prefix / "layout.json")), "cards"));
    EXPECT_EQ(places.size(), 1u);
    EXPECT_TRUE(places.contains("a"));
  }
  EXPECT_EQ(Read(root / "externals/unrelated/layout.json"), "{}");
}
INSTANTIATE_TEST_SUITE_P(SaveAndEdit, ScopedSourceOperation,
                         ::testing::Values(false, true));
}  // namespace
}  // namespace tsugumi::test
