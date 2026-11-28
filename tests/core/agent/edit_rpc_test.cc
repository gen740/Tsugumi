#include "project_fixture.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::test {
namespace {
TEST_F(AgentProjectFixture,
       PutCreatesMissingFilesAndKeepsExplicitExpectations) {
  StartProject();
  const auto conversation = Conversation("put-missing");
  const auto root = directory.path / "project";
  const std::string create =
      R"({"edits":[{"file_path":"definitions/data/_card.json","put":"{\"type\":\"draft\",\"symbol\":\"pencil.and.outline\"}","expect_missing":true},{"file_path":"definitions/data/README.md","put":"---\ntitle: data\ndescription:\n---\n","expect_missing":true},{"file_path":"tsugumi.json","selector":[{"field":"canvas"},{"field":"cards"}],"old_value":{"present":true,"value":[]},"new_value":{"present":true,"value":[{"id":"data"}]}},{"file_path":"definitions/data/src/main.py","put":"original"}]})";
  const auto first = Tool(conversation, "project_edit", create, "create");
  ASSERT_TRUE(first.Code().empty());
  ASSERT_FALSE(JsonBool(first.View(), "/isError")) << first.View();
  EXPECT_EQ(Read(root / "definitions/data/src/main.py"), "original");

  const auto changed = Tool(
      conversation, "project_edit",
      R"({"edits":[{"file_path":"definitions/data/src/main.py","old_string":"original","new_string":"changed"}]})",
      "change");
  ASSERT_TRUE(changed.Code().empty());
  ASSERT_FALSE(JsonBool(changed.View(), "/isError")) << changed.View();
  const auto replay = Tool(conversation, "project_edit", create, "create");
  ASSERT_TRUE(replay.Code().empty());
  EXPECT_EQ(JsonString(replay.View(), "/text"),
            JsonString(first.View(), "/text"));
  EXPECT_EQ(Read(root / "definitions/data/src/main.py"), "changed");

  int id = 0;
  for (
      const auto& edit :
      {R"({"file_path":"definitions/data/src/main.py","put":"new"})",
       R"({"file_path":"definitions/data/src/main.py","put":"new","expect_missing":true})",
       R"({"file_path":"definitions/data/src/main.py","put":"new","expected_hash":"wrong"})",
       R"({"file_path":"definitions/data/src/missing.py","put":"new","expected_hash":"wrong"})"}) {
    const auto refused = Tool(conversation, "project_edit",
                              "{\"edits\":[" + std::string(edit) + "]}",
                              "refused-" + std::to_string(id++));
    ASSERT_TRUE(refused.Code().empty());
    EXPECT_TRUE(JsonBool(refused.View(), "/isError"));
    EXPECT_EQ(JsonString(JsonString(refused.View(), "/text"), "/value/kind"),
              "source_changed");
    EXPECT_EQ(Read(root / "definitions/data/src/main.py"), "changed");
    EXPECT_FALSE(
        std::filesystem::exists(root / "definitions/data/src/missing.py"));
  }
}

TEST_F(AgentProjectFixture, MalformedEditReportsTheFieldWithoutWriting) {
  StartProject();
  const auto conversation = Conversation("edit-decode");
  const auto project = directory.path / "project/tsugumi.json";
  const auto before = Read(project);
  const auto reply =
      Tool(conversation, "project_edit",
           R"({"edits":[{"file_path":"tsugumi.json","selector":[null]}]})");
  ASSERT_TRUE(reply.Code().empty());
  EXPECT_TRUE(JsonBool(reply.View(), "/isError"));
  const auto failure = JsonString(reply.View(), "/text");
  EXPECT_NE(failure.find("edits[0].selector[0]"), std::string::npos) << failure;
  EXPECT_EQ(Read(project), before);
}

TEST_F(AgentProjectFixture,
       CardReferenceDraftEditPreservesIntentAndRebindsWires) {
  namespace json = ::tsugumi::json;
  const auto root = directory.path / "project";
  Write(root / "definitions/start/_card.json",
        R"({"type":"trigger","symbol":"play.circle"})");
  Write(root / "definitions/plan/_card.json",
        R"({"type":"draft","symbol":"none"})");
  const std::string readme =
      "---\ntitle: plan\ndescription: Make a greeting\n---\n\nReturn hello.";
  Write(root / "definitions/plan/README.md", readme);
  Write(root / "definitions/next/_card.json",
        R"({"type":"draft","symbol":"pencil.and.outline"})");
  StartProject(
      false,
      R"({"nixPath": [],"canvas":{"cards":[{"id":"start"},{"id":"plan"},{"id":"next"}],"connections":[{"from":{"card":"start","port":"out"},"to":{"card":"plan"}},{"from":{"card":"plan"},"to":{"card":"next"}}]}})");
  const auto conversation = Conversation("reference-draft-edit");
  const auto reference = Read(std::filesystem::path(TSUGUMI_DEMOS_DIR) /
                              "reference/cards.md");
  const auto fence = reference.find("```json\n");
  ASSERT_NE(fence, std::string::npos);
  const auto begin = fence + std::string("```json\n").size();
  const auto end = reference.find("\n```", begin);
  ASSERT_NE(end, std::string::npos);
  const auto args = reference.substr(begin, end - begin);
  const auto reply = Tool(conversation, "project_edit", args, "implement");
  ASSERT_TRUE(reply.Code().empty()) << reply.View();
  ASSERT_FALSE(JsonBool(reply.View(), "/isError")) << reply.View();
  const auto project = Read(root / "tsugumi.json");
  EXPECT_NE(project.find(R"("nixPath": [])"), std::string::npos);
  EXPECT_EQ(JsonString(project, "/canvas/connections/0/to/port"), "start");
  EXPECT_EQ(JsonString(project, "/canvas/connections/1/from/port"), "result");
  // next stays a Draft, so its end keeps no port.
  EXPECT_FALSE(json::Object(json::Get(
                   json::Object(json::Array(json::Get(
                                    json::Object(json::Get(
                                        json::Object(project), "canvas")),
                                    "connections"))[1]),
                   "to"))
                   .contains("port"));
  EXPECT_EQ(Read(root / "definitions/plan/README.md"), readme);
  const auto definition = Read(root / "definitions/plan/_card.json");
  EXPECT_EQ(JsonString(definition, "/type"), "code");
  EXPECT_TRUE(std::filesystem::exists(root / "definitions/plan/default.nix"));
  EXPECT_TRUE(std::filesystem::exists(root / "definitions/plan/src/main.py"));
  const auto validation = Tool(conversation, "project_validate", "{}");
  ASSERT_TRUE(validation.Code().empty()) << validation.View();
  EXPECT_FALSE(JsonBool(validation.View(), "/isError")) << validation.View();
}

TEST_F(AgentProjectFixture,
       NarrowEditKeepsCanonicalProjectTextAndReplaysTheToolCall) {
  StartProject();
  const auto conversation = Conversation("atomic-edit");
  const auto before = Read(directory.path / "project/tsugumi.json");
  const std::string args =
      R"({"edits":[{"file_path":"tsugumi.json","selector":[{"field":"name"}],"old_value":{"present":true,"value":"PRIVATE_PROJECT_SENTINEL"},"new_value":{"present":true,"value":"Renamed"}}]})";
  const auto reply = Tool(conversation, "project_edit", args, "rename");
  ASSERT_TRUE(reply.Code().empty()) << reply.View();
  ASSERT_FALSE(JsonBool(reply.View(), "/isError")) << reply.View();
  auto expected = before;
  expected.replace(expected.find("PRIVATE_PROJECT_SENTINEL"),
                   std::string("PRIVATE_PROJECT_SENTINEL").size(), "Renamed");
  const auto schema = document_text::Schema::Project;
  expected = document_text::print(
      schema, document_text::parse(schema, "tsugumi.json", expected));
  EXPECT_EQ(Read(directory.path / "project/tsugumi.json"), expected);
  const auto retry = Tool(conversation, "project_edit", args, "rename");
  ASSERT_TRUE(retry.Code().empty()) << retry.View();
  EXPECT_EQ(JsonString(reply.View(), "/text"),
            JsonString(retry.View(), "/text"));
  EXPECT_EQ(Read(directory.path / "project/tsugumi.json"), expected);
  const auto formatted = Tool(
      conversation, "project_edit",
      R"({"edits":[{"file_path":"tsugumi.json","old_string":"\"name\":","new_string":"\"name\" : "}]})",
      "format-name");
  ASSERT_TRUE(formatted.Code().empty()) << formatted.View();
  ASSERT_FALSE(JsonBool(formatted.View(), "/isError")) << formatted.View();
  EXPECT_EQ(Read(directory.path / "project/tsugumi.json"), expected);
  const auto stale =
      Tool(conversation, "project_edit", args, "stale-old-value");
  ASSERT_TRUE(stale.Code().empty()) << stale.View();
  EXPECT_TRUE(JsonBool(stale.View(), "/isError"));
  EXPECT_EQ(Read(directory.path / "project/tsugumi.json"), expected);
}
TEST_F(AgentProjectFixture,
       NarrowFileEditCommitsTextReplacementAndGuardedMoveTogether) {
  StartProject();
  const auto conversation = Conversation("atomic-file-edit");
  const auto created = Tool(
      conversation, "project_edit",
      R"({"edits":[{"file_path":"definitions/data/_card.json","put":"{\"type\":\"draft\",\"symbol\":\"pencil.and.outline\"}","expect_missing":true},{"file_path":"definitions/data/README.md","put":"---\ntitle: data\ndescription:\n---\n","expect_missing":true},{"file_path":"tsugumi.json","selector":[{"field":"canvas"},{"field":"cards"}],"old_value":{"present":true,"value":[]},"new_value":{"present":true,"value":[{"id":"data"}]}},{"file_path":"definitions/data/file.txt","put":"old text","expect_missing":true}]})",
      "create");
  ASSERT_TRUE(created.Code().empty()) << created.View();
  ASSERT_FALSE(JsonBool(created.View(), "/isError")) << created.View();
  const auto edited = Tool(
      conversation, "project_edit",
      R"({"edits":[{"file_path":"definitions/data/file.txt","old_string":"old","new_string":"new"},{"file_path":"definitions/data/file.txt","move_to":"definitions/data/moved.txt","expected_hash":"cb0208b0b1fa06bc59f85c8b2be1e45ff2ef6ddbf0cef02e9f276b8208ea48ab","destination_missing":true}]})",
      "replace");
  ASSERT_TRUE(edited.Code().empty()) << edited.View();
  ASSERT_FALSE(JsonBool(edited.View(), "/isError")) << edited.View();
  EXPECT_EQ(Read(directory.path / "project/definitions/data/moved.txt"),
            "new text");
  EXPECT_FALSE(std::filesystem::exists(directory.path /
                                       "project/definitions/data/file.txt"));
  const auto removed = Tool(
      conversation, "project_edit",
      R"({"edits":[{"file_path":"definitions/data/moved.txt","remove":true,"expected_hash":"cb0208b0b1fa06bc59f85c8b2be1e45ff2ef6ddbf0cef02e9f276b8208ea48ab"}]})",
      "remove");
  ASSERT_FALSE(JsonBool(removed.View(), "/isError")) << removed.View();
  EXPECT_FALSE(std::filesystem::exists(directory.path /
                                       "project/definitions/data/moved.txt"));
}
}  // namespace
}  // namespace tsugumi::test

namespace tsugumi::test {
namespace {
TEST_F(AgentProjectFixture, OperationIdReplaysOnceAndRejectsAnotherPayload) {
  StartProject();
  // A field guard deliberately stops matching after the first application.
  H::ProjectFieldEditT field;
  field.file_path = "tsugumi.json";
  auto selector = std::make_unique<H::ProjectSelectorT>();
  selector->field = "name";
  field.selector.push_back(std::move(selector));
  field.old_value = std::make_unique<H::ProjectValueT>();
  field.old_value->present = true;
  field.old_value->json = "\"PRIVATE_PROJECT_SENTINEL\"";
  field.new_value = std::make_unique<H::ProjectValueT>();
  field.new_value->present = true;
  field.new_value->json = "\"Changed\"";
  H::ProjectEditsT edits;
  auto edit = std::make_unique<H::ProjectEditT>();
  edit->value.Set(field);
  edits.edits.push_back(std::move(edit));
  H::ProjectOperateRequestT request;
  request.project_id = project_id;
  request.op_id = "d7f52370-d5f6-4c4f-8a1f-546eb93090f1";
  request.body.Set(std::move(edits));
  auto send = [&] {
    return Call(*host, H::RequestPayload::ProjectOperateRequest,
                [&](Builder& b) {
                  return H::ProjectOperateRequest::Pack(b, &request).Union();
                });
  };
  const auto first = send();
  ASSERT_TRUE(first.Code().empty()) << first.View();
  const auto* saved = first.HostValue()
                          ->value_as_ProjectOperationResult()
                          ->reply()
                          ->value_as_SaveResult();
  ASSERT_NE(saved, nullptr) << first.View();
  const auto bytes = Read(directory.path / "project/tsugumi.json");
  const auto second = send();
  ASSERT_TRUE(second.Code().empty()) << second.View();
  const auto* replay = second.HostValue()
                           ->value_as_ProjectOperationResult()
                           ->reply()
                           ->value_as_SaveResult();
  ASSERT_NE(replay, nullptr) << second.View();
  EXPECT_TRUE(replay->replayed().value_or(false));
  EXPECT_EQ(saved->rev(), replay->rev());
  EXPECT_EQ(Read(directory.path / "project/tsugumi.json"), bytes);
  request.body.AsProjectEdits()
      ->edits[0]
      ->value.AsProjectFieldEdit()
      ->new_value->json = "\"Other\"";
  EXPECT_EQ(send().Code(), "operation_conflict");
  EXPECT_EQ(Read(directory.path / "project/tsugumi.json"), bytes);
}
TEST_F(AgentProjectFixture,
       ConnectLayoutAndSubgraphSaveTheirCompleteFootprints) {
  Write(directory.path / "project/definitions/a/_card.json",
        R"({"type":"draft","symbol":"pencil.and.outline"})");
  Write(directory.path / "project/definitions/b/_card.json",
        R"({"type":"draft","symbol":"pencil.and.outline"})");
  const std::string readme = "---\ntitle: a\ndescription: Keep my words\n---\n";
  Write(directory.path / "project/definitions/a/README.md", readme);
  Write(directory.path / "project/definitions/a/data.bin",
        std::string("\0\xff", 2));
  StartProject(
      false,
      R"({"name":"Graph","nixPath": [],"canvas":{"cards":[{"id":"a"},{"id":"b"}],"connections":[]}})");
  const auto conversation = Conversation("transform");
  auto connected =
      Tool(conversation, "project_connect",
           R"({"from_card":"a","from_port":"","to_card":"b","to_port":""})",
           "connect");
  ASSERT_TRUE(connected.Code().empty()) << connected.View();
  ASSERT_FALSE(JsonBool(connected.View(), "/isError")) << connected.View();
  EXPECT_NE(Read(directory.path / "project/tsugumi.json").find("\"from\""),
            std::string::npos);
  const auto laid_out =
      Tool(conversation, "project_layout", R"({"action":"arrange"})", "layout");
  ASSERT_TRUE(laid_out.Code().empty()) << laid_out.View();
  ASSERT_FALSE(JsonBool(laid_out.View(), "/isError")) << laid_out.View();
  EXPECT_NE(FixtureLayoutText(Read(directory.path / "project/layout.bin")).find("\"a\""),
            std::string::npos);
  const auto untangled = Tool(conversation, "project_layout",
                              R"({"action":"untangle"})", "untangle");
  ASSERT_FALSE(JsonBool(untangled.View(), "/isError")) << untangled.View();
  const auto nested = Tool(conversation, "project_make_subgraph",
                           R"({"cards":["a","b"],"id":"group"})", "subgraph");
  ASSERT_TRUE(nested.Code().empty()) << nested.View();
  ASSERT_FALSE(JsonBool(nested.View(), "/isError")) << nested.View();
  EXPECT_EQ(
      Read(directory.path / "project/externals/group/definitions/a/data.bin"),
      std::string("\0\xff", 2));
  EXPECT_EQ(
      Read(directory.path / "project/externals/group/definitions/a/README.md"),
      readme);
  EXPECT_FALSE(
      std::filesystem::exists(directory.path / "project/definitions/a"));
  EXPECT_NE(FixtureLayoutText(Read(directory.path / "project/layout.bin")).find("\"group\""),
            std::string::npos);
  EXPECT_NE(FixtureLayoutText(Read(directory.path / "project/externals/group/layout.bin"))
                .find("\"a\""),
            std::string::npos);
}
TEST_F(AgentProjectFixture, NarrowEditRequiresWriteAndPermissionApproval) {
  Write(directory.path / "project/definitions/a/_card.json", FixtureCodeCard());
  StartProject(
      false,
      R"({"name":"Permissions","nixPath": [],"canvas":{"cards":[{"id":"a"}],"connections":[]}})");
  const auto conversation = Conversation("permissions-edit");
  const auto before = Read(directory.path / "project/definitions/a/_card.json");
  const std::string args =
      R"({"edits":[{"file_path":"definitions/a/_card.json","selector":[{"field":"grants"},{"field":"network"}],"old_value":{"present":true,"value":false},"new_value":{"present":true,"value":true}}]})";
  EXPECT_EQ(Tool(conversation, "project_edit", args, "without-write").Code(),
            "decision_required");
  EXPECT_EQ(Read(directory.path / "project/definitions/a/_card.json"), before);
  const auto pending = Tool(conversation, "project_edit", args, "permission");
  EXPECT_EQ(pending.Code(), "decision_required") << pending.View();
  EXPECT_EQ(Read(directory.path / "project/definitions/a/_card.json"), before);
}
}  // namespace
}  // namespace tsugumi::test
