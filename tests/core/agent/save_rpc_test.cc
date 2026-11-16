#include "agent/json.hh"
#include "project_fixture.hh"

namespace tsugumi::test {
namespace {
namespace json = agent::json;
TEST_F(AgentProjectFixture,
       AgentSavePolicyIsAppliedByTheHostAndItsReceiptSurvivesRestart) {
  Write(directory.path / "project/definitions/plan/_card.json",
        R"({"template":"draft"})");
  Write(
      directory.path / "project/definitions/plan/_meta.json",
      R"({"description":"Plan","content":"Keep these words","comments":[{"id":"c1","text":"Ask","author":"user"}]})");
  StartProject();
  const auto conversation = Conversation("save-policy");
  Grant(conversation);
  Grant(conversation, Tsugumi::Service::GrantScope::Conversation,
        Tsugumi::Service::Access::Write);
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
  EXPECT_EQ(json::Text(json::Object(comments[0]), "author"), "user");
  EXPECT_EQ(json::Text(json::Object(comments[0]), "stateBy"), "agent");
  EXPECT_EQ(json::Text(json::Object(comments[1]), "author"), "agent");
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
TEST_F(AgentProjectFixture,
       ParallelScopedCreatorsMergeAndKeepAnInterveningEdit) {
  namespace json = agent::json;
  StartProject();
  const auto first = Conversation("parallel-creator-a"),
             second = Conversation("parallel-creator-b");
  for (const auto& conversation : {first, second}) {
    Grant(conversation);
    Grant(conversation, Tsugumi::Service::GrantScope::Conversation,
          Tsugumi::Service::Access::Write);
  }
  const auto initial = json::Object(json::Get(
      json::Object(JsonString(Inspect(first).View(), "/text")), "value"));
  auto seed_project = json::Object(json::Text(initial, "project_json"));
  seed_project["canvas"] = R"({"cards":[{"id":"upstream"}],"connections":[]})";
  const auto seeded = Tool(
      first, "project_save",
      json::Encode(json::Members{
          {"base_rev", json::Get(initial, "rev")},
          {"changes",
           json::Encode(std::vector<std::string>{
               json::Encode(json::Members{
                   {"value_type", json::Quote("SetProject")},
                   {"value",
                    json::Encode(json::Members{
                        {"json", json::Quote(json::Encode(seed_project))}})}}),
               json::Encode(json::Members{
                   {"value_type", json::Quote("PutDefinition")},
                   {"value",
                    json::Encode(json::Members{
                        {"id", json::Quote("upstream")},
                        {"card_json",
                         json::Quote(R"({"template":"draft"})")}})}})})}}),
      "seed-upstream");
  ASSERT_FALSE(JsonBool(seeded.View(), "/isError"));
  const auto snapshot = json::Object(json::Get(
      json::Object(JsonString(Inspect(first).View(), "/text")), "value"));
  const auto rev = json::Get(snapshot, "rev");
  ModelServer a, b;
  Configure(a, "fixture");
  Configure(b, "anthropic");
  for (const auto& [conversation, provider] :
       {std::pair{first, "fixture"}, std::pair{second, "anthropic"}}) {
    const auto configured = Call(
        *human, A::RequestPayload::ConfigureRequest, [&](Builder& builder) {
          Tsugumi::Service::ProviderSelectionT selection;
          selection.profile = "desktop";
          selection.provider_id = provider;
          return A::CreateConfigureRequest(
                     builder,
                     Tsugumi::Service::ProviderSelection::Pack(builder,
                                                               &selection),
                     builder.CreateString(conversation))
              .Union();
        });
    ASSERT_TRUE(configured.Code().empty());
  }
  a.Queue(AnthropicTool("reserve", "card_id", R"({"name":"First helper"})"));
  b.Queue(AnthropicTool("reserve", "card_id", R"({"name":"Second helper"})"));
  auto delegate = [&](const std::string& conversation) {
    return Tool(
        conversation, "delegate",
        R"({"tasks":[{"name":"creator","instruction":"Add your reserved Card to the workflow.","read_cards":["upstream"],"write_cards":[],"run_cards":[]}]})",
        "parallel-delegate");
  };
  auto fa = std::async(std::launch::async, [&] { return delegate(first); });
  auto fb = std::async(std::launch::async, [&] { return delegate(second); });
  const std::shared_ptr<void> finish_a(
      &a, [&](void*) { a.Queue(AnthropicText("End.")); });
  const std::shared_ptr<void> finish_b(
      &b, [&](void*) { b.Queue(AnthropicText("End.")); });
  ASSERT_TRUE(a.WaitForRequests(2));
  ASSERT_TRUE(b.WaitForRequests(2));
  const auto id_a =
      JsonString(JsonString(a.Requests()[1], "/messages/2/content/0/content"),
                 "/value/id");
  const auto id_b =
      JsonString(JsonString(b.Requests()[1], "/messages/2/content/0/content"),
                 "/value/id");
  ASSERT_FALSE(id_a.empty());
  ASSERT_FALSE(id_b.empty());
  auto person_project = json::Object(json::Text(snapshot, "project_json"));
  person_project["name"] = json::Quote("The person's concurrent edit");
  const auto person = Tool(
      first, "project_save",
      json::Encode(json::Members{
          {"base_rev", rev},
          {"changes",
           json::Encode(std::vector<std::string>{json::Encode(json::Members{
               {"value_type", json::Quote("SetProject")},
               {"value", json::Encode(json::Members{
                             {"json", json::Quote(json::Encode(
                                          person_project))}})}})})}}),
      "person-edit");
  ASSERT_FALSE(JsonBool(person.View(), "/isError")) << person.View();
  for (const auto& [model, id] : {std::pair{&a, id_a}, std::pair{&b, id_b}}) {
    auto project = json::Object(json::Text(snapshot, "project_json"));
    project["canvas"] = json::Encode(json::Members{
        {"cards", json::Encode(std::vector<std::string>{
                      R"({"id":"upstream"})",
                      json::Encode(json::Members{{"id", json::Quote(id)}})})},
        {"connections",
         json::Encode(std::vector<std::string>{json::Encode(json::Members{
             {"from", R"({"card":"upstream","port":""})"},
             {"to",
              json::Encode(json::Members{{"card", json::Quote(id)},
                                         {"port", json::Quote("")}})}})})}});
    const auto args = json::Encode(json::Members{
        {"base_rev", rev},
        {"changes",
         json::Encode(std::vector<std::string>{
             json::Encode(json::Members{
                 {"value_type", json::Quote("SetProject")},
                 {"value",
                  json::Encode(json::Members{
                      {"json", json::Quote(json::Encode(project))}})}}),
             json::Encode(json::Members{
                 {"value_type", json::Quote("PutDefinition")},
                 {"value",
                  json::Encode(json::Members{
                      {"id", json::Quote(id)},
                      {"card_json",
                       json::Quote(R"({"template":"draft"})")}})}})})}});
    model->Queue(AnthropicTool("save-card", "project_save", args));
    model->Queue(AnthropicText("Saved."));
  }
  const auto ra = fa.get(), rb = fb.get();
  ASSERT_TRUE(ra.Code().empty());
  ASSERT_TRUE(rb.Code().empty());
  for (auto* model : {&a, &b}) {
    const auto requests = model->Requests();
    ASSERT_EQ(requests.size(), 3u);
    EXPECT_FALSE(JsonBool(requests[2], "/messages/4/content/0/is_error"))
        << JsonString(requests[2], "/messages/4/content/0/content");
  }
  const auto final = json::Object(json::Get(
      json::Object(JsonString(Inspect(first).View(), "/text")), "value"));
  const auto project = json::Object(json::Text(final, "project_json"));
  EXPECT_EQ(json::Text(project, "name"), "The person's concurrent edit");
  EXPECT_EQ(json::Array(json::Get(json::Object(project.at("canvas")), "cards"))
                .size(),
            3u);
  EXPECT_EQ(
      json::Array(json::Get(json::Object(project.at("canvas")), "connections"))
          .size(),
      2u);
  const auto positions =
      json::Object(json::Object(json::Text(final, "layout_json")).at("cards"));
  EXPECT_TRUE(positions.contains(id_a));
  EXPECT_TRUE(positions.contains(id_b));
  EXPECT_NE(positions.at(id_a), positions.at(id_b));
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

TEST_F(AgentProjectFixture, ReturnedSaveRevisionIsACompleteMergeBase) {
  Write(directory.path / "project/definitions/new/_card.json",
        R"({"template":"draft"})");
  StartProject();
  const auto agent = Conversation("save-chain");
  Grant(agent);
  Grant(agent, Tsugumi::Service::GrantScope::Conversation,
        Tsugumi::Service::Access::Write);
  const auto initial = json::Object(json::Get(
      json::Object(JsonString(Inspect(agent).View(), "/text")), "value"));
  auto save = [&](const std::string& conversation, const std::string& rev,
                  const std::string& document, const std::string& id) {
    return Tool(
        conversation, "project_save",
        json::Encode(json::Members{
            {"base_rev", rev},
            {"changes",
             json::Encode(std::vector<std::string>{json::Encode(json::Members{
                 {"value_type", json::Quote("SetProject")},
                 {"value", json::Encode(json::Members{
                               {"json", json::Quote(document)}})}})})}}),
        id);
  };
  auto first_doc = json::Object(json::Text(initial, "project_json"));
  first_doc["name"] = json::Quote("Agent's first save");
  const auto first =
      save(agent, json::Get(initial, "rev"), json::Encode(first_doc), "first");
  ASSERT_FALSE(JsonBool(first.View(), "/isError")) << first.View();
  const auto returned_rev =
      json::Get(json::Object(json::Get(
                    json::Object(JsonString(first.View(), "/text")), "value")),
                "rev");
  // No inspect by the Agent between saves. Another editor changes the name.
  // Use the UI's direct project RPC. An Agent inspect/save by the other
  // editor would itself populate the host's global save_bases and mask the bug.
  client::Options options;
  options.endpoint = "unix:" + JsonString(project_ready, "/unix_socket");
  client::Connection editor(options);
  ASSERT_FALSE(editor.Start().get());
  const auto hello =
      editor
          .Call([&](uint64_t id) {
            namespace R = Tsugumi::RPC;
            Builder b;
            const auto input = R::CreateSessionHelloRequest(
                b, 1, b.CreateString("00000000-0000-4000-8000-000000000001"),
                b.CreateString(project_id),
                b.CreateString(JsonString(project_ready, "/token")), 1);
            const auto request = R::CreateRequest(
                b, R::RequestPayload::SessionHelloRequest, input.Union());
            R::FinishMessageBuffer(
                b,
                R::CreateMessage(b, id, R::Payload::Request, request.Union()));
            return client::Bytes(b.GetBufferPointer(),
                                 b.GetBufferPointer() + b.GetSize());
          })
          .get();
  ASSERT_FALSE(hello.error) << hello.error.message;
  ASSERT_NE(Tsugumi::RPC::GetMessage(hello.bytes.data())
                ->payload_as_Reply()
                ->value_as_HelloResult(),
            nullptr);
  auto person_doc = first_doc;
  person_doc["name"] = json::Quote("Person's edit");
  const auto person_result =
      editor
          .Call([&](uint64_t id) {
            namespace R = Tsugumi::RPC;
            R::ProjectSaveRequestT save;
            save.base_rev =
                json::UInt(json::String(returned_rev, returned_rev));
            save.op_id = "00000000-0000-4000-8000-000000000002";
            save.origin = "ui";
            R::SetProjectT document;
            document.json = json::Encode(person_doc);
            auto change = std::make_unique<R::DocumentChangeT>();
            change->value.Set(std::move(document));
            save.changes.push_back(std::move(change));
            Builder b;
            const auto input = R::ProjectSaveRequest::Pack(b, &save);
            const auto request = R::CreateRequest(
                b, R::RequestPayload::ProjectSaveRequest, input.Union());
            R::FinishMessageBuffer(
                b,
                R::CreateMessage(b, id, R::Payload::Request, request.Union()));
            return client::Bytes(b.GetBufferPointer(),
                                 b.GetBufferPointer() + b.GetSize());
          })
          .get();
  ASSERT_FALSE(person_result.error) << person_result.error.message;
  ASSERT_NE(Tsugumi::RPC::GetMessage(person_result.bytes.data())
                ->payload_as_Reply()
                ->value_as_SaveResult(),
            nullptr);
  auto independent = first_doc;
  independent["canvas"] = R"({"cards":[{"id":"new"}],"connections":[]})";
  const auto merged =
      save(agent, returned_rev, json::Encode(independent), "independent");
  ASSERT_FALSE(JsonBool(merged.View(), "/isError")) << merged.View();
  const auto final_doc = Read(directory.path / "project/tsugumi.json");
  EXPECT_EQ(JsonString(final_doc, "/name"), "Person's edit");
  EXPECT_EQ(JsonString(final_doc, "/canvas/cards/0/id"), "new");
  auto collision = first_doc;
  collision["name"] = json::Quote("Agent's colliding edit");
  const auto refused =
      save(agent, returned_rev, json::Encode(collision), "collision");
  ASSERT_TRUE(JsonBool(refused.View(), "/isError")) << refused.View();
  const auto failure = JsonString(refused.View(), "/text");
  EXPECT_EQ(JsonString(failure, "/value/kind"), "revision_conflict");
  EXPECT_NE(JsonString(failure, "/value/message").find("name"),
            std::string::npos);
  EXPECT_EQ(Read(directory.path / "project/tsugumi.json"), final_doc);
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
  Grant(conversation, Tsugumi::Service::GrantScope::Conversation,
        Tsugumi::Service::Access::Write);
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
      "save", "project_save",
      json::Encode(json::Members{
          {"base_rev", json::Get(snapshot, "rev")},
          {"changes",
           json::Encode(std::vector<std::string>{
               json::Encode(json::Members{
                   {"value_type", json::Quote("PutSource")},
                   {"value",
                    json::Encode(json::Members{
                        {"path", json::Quote("externals/storage/definitions/"
                                             "child/src/main.py")},
                        {"bytes", json::Quote("changed\n")},
                        {"base_hash", json::Quote(hash)}})}}),
               R"({"value_type":"PutDefinition","value":{"id":"writer","card_json":"{\"template\":\"draft\",\"network\":true}","keep_meta":true}})"})}})));
  model.Queue(AnthropicText("Finished."));
  auto delegated = std::async(std::launch::async, [&] {
    return Tool(
        conversation, "delegate",
        R"({"tasks":[{"name":"scoped","instruction":"Save the supplied edits.","read_cards":["outer/child","writer"],"write_cards":["outer/child","writer"],"run_cards":[]}]})",
        "delegate");
  });
  // Always unblock approval on an assertion failure before joining the helper.
  std::string approval;
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
  // helper waits. A CAS merge alone would preserve this link and leak its edit.
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
    if (json::Text(failure, "message").find("Helper scope denied") !=
        std::string::npos)
      scope_denied = true;
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
  namespace json = agent::json;
  const auto nested = directory.path / "project/externals/storage";
  const std::string document =
      R"({"name":"Nested","pkgsPath":"tsugumi:default","boundary":{"inputs":{},"outputs":{}},"canvas":{"cards":[{"id":"upstream"},{"id":"secret"}],"connections":[]}})";
  Write(nested / "tsugumi.json", document);
  Write(nested / "definitions/upstream/_card.json", R"({"template":"draft"})");
  Write(nested / "definitions/secret/_card.json", R"({"template":"draft"})");
  StartProject();
  const auto conversation = Conversation("nested-creator");
  Grant(conversation);
  Grant(conversation, Tsugumi::Service::GrantScope::Conversation,
        Tsugumi::Service::Access::Write);
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
  std::string hash;
  for (const auto &raw : json::Array(json::Get(snapshot, "sources"))) {
    const auto source = json::Object(raw);
    if (json::Text(source, "path") == "externals/storage/tsugumi.json")
      hash = json::Text(source, "hash");
  }
  ASSERT_FALSE(hash.empty());
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
      &model, [&](void *) { model.Queue(AnthropicText("End.")); });
  ASSERT_TRUE(model.WaitForRequests(2));
  const auto reserved = JsonString(model.Requests()[1], "/messages/2/content/0/content");
  ASSERT_TRUE(reserved.starts_with('{')) << reserved;
  const auto id = JsonString(reserved, "/value/id");
  ASSERT_FALSE(id.empty());
  auto scoped = json::Object(document);
  scoped["canvas"] = json::Encode(json::Members{
      {"cards", json::Encode(std::vector<std::string>{
                    R"({"id":"upstream"})",
                    json::Encode(json::Members{{"id", json::Quote(id)}})})},
      {"connections",
       json::Encode(std::vector<std::string>{json::Encode(json::Members{
           {"from", R"({"card":"upstream","port":""})"},
           {"to",
            json::Encode(json::Members{{"card", json::Quote(id)},
                                       {"port", json::Quote("")}})}})})}});
  const auto args = json::Encode(json::Members{
      {"base_rev", rev},
      {"changes",
       json::Encode(std::vector<std::string>{
           json::Encode(json::Members{
               {"value_type", json::Quote("PutSource")},
               {"value",
                json::Encode(json::Members{
                    {"path", json::Quote("externals/storage/tsugumi.json")},
                    {"bytes", json::Quote(json::Encode(scoped))},
                    {"base_hash", json::Quote(hash)}})}}),
           json::Encode(json::Members{
               {"value_type", json::Quote("PutSource")},
               {"value",
                json::Encode(json::Members{
                    {"path", json::Quote("externals/storage/definitions/" + id +
                                         "/_card.json")},
                    {"bytes", json::Quote(R"({"template":"draft"})")}})}})})}});
  model.Queue(AnthropicTool("save", "project_save", args));
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
  for (const auto &[key, value] : json::Object(before_layout.at("cards")))
    EXPECT_EQ(positions.at(key), value);
  EXPECT_NE(Read(nested / "tsugumi.json").find("secret"), std::string::npos);
  EXPECT_EQ(response.find("secret"), std::string::npos);
}
} // namespace
} // namespace tsugumi::test
