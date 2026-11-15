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
