#include "project_fixture.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::test {
namespace {
namespace json = ::tsugumi::json;
TEST_F(AgentProjectFixture,
       AgentSavePolicyIsAppliedByTheHostAndItsReceiptSurvivesRestart) {
  Write(directory.path / "project/definitions/plan/_card.json",
        R"({"type":"draft","symbol":"pencil.and.outline"})");
  Write(directory.path / "project/definitions/plan/README.md",
        "---\ntitle: plan\ndescription: Plan\n---\n\nKeep these words");
  Write(directory.path / "project/definitions/plan/_comments.json",
        R"({"comments":[{"id":"c1","text":"Ask","author":"user"}]})");
  StartProject(
      false,
      R"({"name":"PRIVATE_PROJECT_SENTINEL","agentExclude":[],"nixPath": [],"canvas":{"cards":[],"connections":[]}})");
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
  json::Members value{
      {"id", json::Quote("plan")},
      {"card_json", json::Quote(::tsugumi::test::FixtureCodeCard())},
      {"keep_comments", "true"}};
  const auto refused =
      Tool(conversation, "project_save", save(value), "refused");
  ASSERT_TRUE(refused.Code().empty()) << refused.Code();
  EXPECT_TRUE(JsonBool(refused.View(), "/isError"));
  EXPECT_EQ(JsonString(JsonString(refused.View(), "/text"), "/value/kind"),
            "validation_failed");
  // A policy refusal is a terminal host receipt, not an interrupted core save.
  EXPECT_EQ(Tool(conversation, "project_save", save(value), "refused").View(),
            refused.View());
  value["keep_readme"] = "true";
  value["comments_json"] = json::Quote(
      R"({"comments":[{"id":"c1","text":"Ask","author":"agent","resolved":true,"state_by":"user","resolution":"Done"},{"text":"New","author":"user"}]})");
  const auto args = save(value);
  const auto saved = Tool(conversation, "project_save", args, "saved");
  ASSERT_TRUE(saved.Code().empty());
  EXPECT_FALSE(JsonBool(saved.View(), "/isError"));
  const auto body = JsonString(saved.View(), "/text");
  EXPECT_EQ(JsonString(body, "/value/comments/0/card"), "plan");
  EXPECT_EQ(JsonString(body, "/value/comments/0/by"), "agent");
  auto meta = json::Object(
      Read(directory.path / "project/definitions/plan/_comments.json"));
  auto comments = json::Array(json::Get(meta, "comments"));
  EXPECT_EQ(json::Text(json::Object(comments[0]), "state_by"), "agent");
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
TEST_F(AgentProjectFixture, ReadsTheConfirmedExternalDefinitionUntilReload) {
  const auto nested = directory.path / "project/externals/outer";
  Write(nested / "tsugumi.json",
        R"({"name":"Nested","canvas":{"cards":[],"connections":[]}})");
  const auto definition = nested / "definitions/child/_card.json";
  const std::string accepted =
      R"({"type":"draft","symbol":"pencil.and.outline"})";
  Write(definition, accepted);
  Write(directory.path / "project/definitions/outer/_card.json",
        R"({"type":"subgraph","symbol":"link"})");
  StartProject();
  const auto conversation = Conversation("read-confirmed-document");
  Grant(conversation);
  ASSERT_TRUE(Inspect(conversation).Code().empty());
  Write(definition, ::tsugumi::test::FixtureCodeCard(
                        R"({"grants":{"network":true,"gui":false}})"));
  ASSERT_TRUE(Inspect(conversation).Code().empty());
  const auto reply =
      Tool(conversation, "project_file_read",
           R"({"path":"externals/outer/definitions/child/_card.json"})");
  ASSERT_TRUE(reply.Code().empty()) << reply.View();
  EXPECT_FALSE(JsonBool(reply.View(), "/isError")) << reply.View();
  EXPECT_EQ(JsonString(JsonString(reply.View(), "/text"), "/value/bytes"),
            accepted);
}

TEST_F(AgentProjectFixture, ExclusionsUseAcceptedAndRecordedDefinitionPaths) {
  const auto root = directory.path / "project";
  Write(root / "definitions/private/_card.json",
        R"({"type":"subgraph","symbol":"link"})");
  Write(root / "externals/private/tsugumi.json",
        FixtureProjectDocument(
            R"({"canvas":{"cards":[{"id":"leaf"}],"connections":[]}})"));
  Write(root / "externals/private/definitions/leaf/_card.json",
        R"({"type":"trigger","symbol":"play.circle"})");
  StartProject(
      false,
      R"({"nixPath": [],"agentExclude":["externals/private/definitions/leaf/_card.json"],"canvas":{"cards":[{"id":"private"}],"connections":[]}})");
  const auto conversation = Conversation("confirmed-exclusions");
  for (const auto access :
       {Tsugumi::Service::Access::Read, Tsugumi::Service::Access::Run})
    Grant(conversation, access);
  const auto revision = JsonString(
      JsonString(Inspect(conversation).View(), "/text"), "/value/rev");
  auto edited = json::Object(Read(root / "tsugumi.json"));
  edited["canvas"] = R"({"cards":[{"id":"public"}],"connections":[]})";
  std::filesystem::rename(root / "definitions/private",
                          root / "definitions/public");
  std::filesystem::rename(root / "externals/private",
                          root / "externals/public");
  Write(root / "tsugumi.json", json::Encode(edited));
  const auto request =
      "{\"base_rev\":" + Quote(revision) +
      R"(,"card":{"segments":["private","leaf"]},"propagate":false})";
  const auto run = Tool(conversation, "run", request);
  EXPECT_EQ(run.Code(), "not_found");

  // A human can run the same Card. The run retains the accepted physical path,
  // even after the renamed subgraph has been reloaded and the current path is
  // public.
  namespace R = Tsugumi::RPC;
  client::Options options;
  options.endpoint = "unix:" + JsonString(project_ready, "/unix_socket");
  std::promise<std::string> finished;
  auto completion = finished.get_future();
  client::Connection core(options);
  core.OnEvent([&](client::Bytes bytes) {
    const auto* note = R::GetMessage(bytes.data())->payload_as_Notification();
    const auto* event = note ? note->value_as_RunEvent() : nullptr;
    if (event && event->value_as_RunFinished())
      finished.set_value(event->run_id()->str());
  });
  ASSERT_FALSE(core.Start().get());
  const auto call = [&](R::RequestPayload type, const auto& value) {
    auto result =
        core.Call([&](uint64_t id) {
              Builder b;
              const auto request = R::CreateRequest(b, type, value(b));
              R::FinishMessageBuffer(
                  b, R::CreateMessage(b, id, R::Payload::Request,
                                      request.Union()));
              return client::Bytes(b.GetBufferPointer(),
                                   b.GetBufferPointer() + b.GetSize());
            })
            .get();
    if (result.error) throw std::runtime_error(result.error.message);
    return result.bytes;
  };
  const auto hello =
      call(R::RequestPayload::SessionHelloRequest, [&](Builder& b) {
        return R::CreateSessionHelloRequest(
                   b, b.CreateString(project_id),
                   b.CreateString(JsonString(project_ready, "/token")))
            .Union();
      });
  ASSERT_NE(
      R::GetMessage(hello.data())->payload_as_Reply()->value_as_HelloResult(),
      nullptr);
  const auto opened =
      call(R::RequestPayload::ProjectOpenRequest,
           [&](Builder& b) { return R::CreateProjectOpenRequest(b).Union(); });
  ASSERT_NE(
      R::GetMessage(opened.data())->payload_as_Reply()->value_as_OpenResult(),
      nullptr);
  const auto accepted = call(R::RequestPayload::RunRequest, [&](Builder& b) {
    const auto card = Tsugumi::Data::CreateCardPath(
        b,
        b.CreateVectorOfStrings(std::vector<std::string>{"private", "leaf"}));
    return R::CreateRunRequest(b, 0, b.CreateString("human-run"),
                               b.CreateString("test"), card, false)
        .Union();
  });
  const auto* started = R::GetMessage(accepted.data())
                            ->payload_as_Reply()
                            ->value_as_RunAccepted();
  ASSERT_NE(started, nullptr);
  const auto run_id = started->run()->run_id()->str();
  const auto raw_record = "{\"run_id\":" + Quote(run_id) +
                          R"(,"card":{"segments":["private","leaf"]}})";
  EXPECT_EQ(Tool(conversation, "run_record", raw_record).Code(), "not_found");

  // Wait for a real terminal event before reloading the rename.
  ASSERT_EQ(completion.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(completion.get(), run_id);
  const auto reloaded =
      call(R::RequestPayload::ProjectReloadRequest, [&](Builder& b) {
        return R::CreateProjectReloadRequest(
                   b, 0, b.CreateString("human-reload"), b.CreateString("test"))
            .Union();
      });
  ASSERT_NE(
      R::GetMessage(reloaded.data())->payload_as_Reply()->value_as_SaveResult(),
      nullptr);
  const auto paths =
      call(R::RequestPayload::RunDefinitionPathsRequest, [&](Builder& b) {
        const auto card = Tsugumi::Data::CreateCardPath(
            b, b.CreateVectorOfStrings(
                   std::vector<std::string>{"private", "leaf"}));
        return R::CreateRunDefinitionPathsRequest(b, b.CreateString(run_id),
                                                  card)
            .Union();
      });
  const auto* archived = R::GetMessage(paths.data())
                             ->payload_as_Reply()
                             ->value_as_RunDefinitionPaths();
  ASSERT_NE(archived, nullptr);
  EXPECT_TRUE(std::ranges::any_of(*archived->paths(), [](const auto* file) {
    return file->string_view() ==
           "externals/private/definitions/leaf/_card.json";
  }));
  EXPECT_EQ(Tool(conversation, "run_record", raw_record).Code(), "not_found");
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

TEST_F(AgentProjectFixture, NestedScopedCreatorSavesGraphWithoutWritingLayout) {
  namespace json = ::tsugumi::json;
  const auto nested = directory.path / "project/externals/outer";
  const std::string document =
      R"({"name":"Nested","nixPath": [],"boundary":{"inputs":[],"outputs":[]},"canvas":{"cards":[{"id":"upstream"},{"id":"secret"}],"connections":[]}})";
  Write(nested / "tsugumi.json", document);
  Write(nested / "definitions/upstream/_card.json",
        R"({"type":"draft","symbol":"pencil.and.outline"})");
  Write(nested / "definitions/secret/_card.json",
        R"({"type":"draft","symbol":"pencil.and.outline"})");
  Write(directory.path / "project/definitions/outer/_card.json",
        R"({"type":"subgraph","symbol":"link"})");
  StartProject();
  const auto conversation = Conversation("nested-creator");
  Grant(conversation);
  Grant(conversation, Tsugumi::Service::Access::Write);
  const auto snapshot = json::Object(
      json::Get(json::Object(JsonString(Inspect(conversation).View(), "/text")),
                "value"));
  const auto rev = json::Get(snapshot, "rev");
  ModelServer model;
  Configure(model);
  model.Queue(AnthropicTool(
      "reserve", "card_id",
      R"({"name":"Nested helper","project_path":"externals/outer/"})"));
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
      {"from", R"({"card":"upstream"})"},
      {"to", json::Encode(json::Members{{"card", json::Quote(id)},
                                        {"port", json::Quote("")}})}});
  const auto edit = [&](const std::string& member, const std::string& value) {
    return json::Encode(json::Members{
        {"file_path", json::Quote("externals/outer/tsugumi.json")},
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
               {"file_path", json::Quote("externals/outer/definitions/" + id +
                                         "/_card.json")},
               {"put",
                json::Quote(
                    R"({"type":"draft","symbol":"pencil.and.outline"})")},
               {"expect_missing", "true"}}),
           json::Encode(json::Members{
               {"file_path", json::Quote("externals/outer/definitions/" + id +
                                         "/README.md")},
               {"put", json::Quote(host::CardReadme(id))},
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
  EXPECT_FALSE(std::filesystem::exists(nested / "layout.bin"));
  const auto canvas =
      json::Object(json::Object(Read(nested / "tsugumi.json")).at("canvas"));
  const auto cards = json::Array(canvas.at("cards"));
  for (const auto& expected : {id, std::string("secret")})
    EXPECT_TRUE(std::any_of(cards.begin(), cards.end(), [&](const auto& card) {
      return json::Text(json::Object(card), "id") == expected;
    }));
  EXPECT_EQ(response.find("secret"), std::string::npos);
}
}  // namespace
}  // namespace tsugumi::test

namespace tsugumi::test {
namespace {
class ScopedSourceOperation : public AgentProjectFixture,
                              public ::testing::WithParamInterface<bool> {};
TEST_P(ScopedSourceOperation, SourceEditsLeaveLayoutsUntouched) {
  const auto root = directory.path / "project";
  const std::string graph =
      R"({"name":"Nested","nixPath": [],"canvas":{"cards":[{"id":"a"},{"id":"b"}],"connections":[]}})";
  for (const auto& prefix : {std::string{}, std::string{"externals/outer/"},
                             std::string{"externals/unrelated/"}}) {
    for (const auto* id : {"a", "b"})
      Write(root / prefix / "definitions" / id / "_card.json",
            R"({"type":"draft","symbol":"pencil.and.outline"})");
    if (!prefix.empty()) {
      Write(root / prefix / "tsugumi.json", graph);
      Write(root / prefix / "layout.bin", FixtureLayout("{}"));
    }
  }
  for (const auto* id : {"outer", "unrelated"})
    Write(root / "definitions" / id / "_card.json",
          R"({"type":"subgraph","symbol":"link"})");
  StartProject(
      false,
      R"({"name":"Scoped","nixPath": [],"canvas":{"cards":[{"id":"a"},{"id":"b"},{"id":"outer"}],"connections":[]}})");
  const auto conversation = Conversation("source-only-scoped");
  Grant(conversation);
  // A read grant never authorizes a save.
  const auto tool = GetParam() ? "project_edit" : "project_save";
  const auto snapshot = json::Object(
      json::Get(json::Object(JsonString(Inspect(conversation).View(), "/text")),
                "value"));
  const std::string args =
      GetParam()
          ? R"json({"edits":[{"file_path":"definitions/a/code.py","put":"print(1)","expect_missing":true},{"file_path":"externals/outer/definitions/a/code.py","put":"print(2)","expect_missing":true}]})json"
          : json::Encode(json::Members{
                {"base_rev", json::Get(snapshot, "rev")},
                {"changes",
                 R"json([{"value_type":"PutSource","value":{"path":"definitions/a/code.py","bytes":"print(1)"}},{"value_type":"PutSource","value":{"path":"externals/outer/definitions/a/code.py","bytes":"print(2)"}}])json"}});
  const auto refused = Tool(conversation, tool, args, "no-write");
  EXPECT_EQ(refused.Code(), "grant_required");
  EXPECT_EQ(Read(root / "layout.bin"), FixtureLayout("{}"));
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
  for (const auto& prefix : {std::string{}, std::string{"externals/outer/"}}) {
    EXPECT_EQ(Read(root / prefix / "definitions/a/code.py"),
              prefix.empty() ? "print(1)" : "print(2)");
    EXPECT_EQ(Read(root / prefix / "layout.bin"), FixtureLayout("{}"));
  }
  EXPECT_EQ(Read(root / "externals/unrelated/layout.bin"), FixtureLayout("{}"));
}
INSTANTIATE_TEST_SUITE_P(SaveAndEdit, ScopedSourceOperation,
                         ::testing::Values(false, true));
}  // namespace
}  // namespace tsugumi::test
