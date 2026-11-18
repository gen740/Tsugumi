#include <set>
#include <tuple>

#include "project_fixture.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::test {
namespace {
namespace json = ::tsugumi::json;

class MnistSaveFixture
    : public AgentProjectFixture,
      public testing::WithParamInterface<std::tuple<std::string, bool>> {};

TEST_P(MnistSaveFixture, GenerateCardsSavesAndReturnsGeometry) {
  const auto demo = std::filesystem::path(TSUGUMI_GUIDE_TEST_ROOT) / "demos" /
                    std::get<0>(GetParam()) / "mnist-classifiers-draft";
  const auto root = directory.path / "project";
  // Bundled fixtures in the Nix store are read-only; the opened project is a
  // writable copy, as it is when the app opens a demo.
  for (const auto& file : std::filesystem::recursive_directory_iterator(demo))
    if (file.is_regular_file())
      Write(root / std::filesystem::relative(file.path(), demo),
            Read(file.path()));
  const auto document = Read(root / "tsugumi.json");
  StartProject(false, document,
               std::get<1>(GetParam()) ? "{}" : Read(root / "layout.json"));
  const auto conversation = Conversation("mnist-generate-cards");
  Grant(conversation);
  Grant(conversation, Tsugumi::Service::Access::Write);
  const auto snapshot = json::Object(
      json::Get(json::Object(JsonString(Inspect(conversation).View(), "/text")),
                "value"));
  auto project = json::Object(document);
  auto canvas = json::Object(project.at("canvas"));
  auto cards = json::Array(canvas.at("cards"));
  cards.push_back(R"({"id":"report"})");
  canvas["cards"] = json::Encode(cards);
  auto wires = json::Array(canvas.at("connections"));
  for (auto& raw : wires) {
    auto wire = json::Object(raw);
    for (const auto* side : {"from", "to"}) {
      auto endpoint = json::Object(wire.at(side));
      if (json::Text(endpoint, "port").empty())
        endpoint["port"] =
            Quote(std::string_view(side) == "to"
                      ? json::Text(json::Object(wire.at("from")), "card")
                      : "data");
      wire[side] = json::Encode(endpoint);
    }
    raw = json::Encode(wire);
  }
  canvas["connections"] = json::Encode(wires);
  project["canvas"] = json::Encode(canvas);
  std::vector<std::string> changes;
  auto change = [&](const std::string& type, json::Members value) {
    changes.push_back(json::Encode(json::Members{
        {"value_type", Quote(type)}, {"value", json::Encode(value)}}));
  };
  change("SetProject", {{"json", Quote(json::Encode(project))}});
  size_t drafts = 0;
  for (const auto& raw : cards) {
    const auto id = json::Text(json::Object(raw), "id");
    if (id != "report" &&
        json::Text(json::Object(Read(root / "definitions" / id / "_card.json")),
                   "template") != "draft")
      continue;
    ++drafts;
    auto meta =
        id == "report"
            ? json::Members{{"description", Quote("Compare the classifiers")},
                            {"content", Quote("Present the comparison.")}}
            : json::Object(Read(root / "definitions" / id / "_meta.json"));
    meta["context"] =
        Quote("Implement this Draft for the MNIST classifier comparison.");
    std::set<std::string> inputs, outputs;
    for (const auto& wire_raw : wires) {
      const auto wire = json::Object(wire_raw);
      const auto from = json::Object(wire.at("from"));
      const auto to = json::Object(wire.at("to"));
      if (json::Text(from, "card") == id)
        outputs.insert(json::Text(from, "port"));
      if (json::Text(to, "card") == id) inputs.insert(json::Text(to, "port"));
    }
    std::vector<std::string> input_defs;
    for (const auto& name : inputs)
      input_defs.push_back(json::Encode(json::Members{{"name", Quote(name)}}));
    const auto definition = json::Encode(json::Members{
        {"template", Quote("code")},
        {"language", Quote("python")},
        {"inputs", json::Encode(input_defs)},
        {"params",
         R"([{"name":"rate","type":"double","data":0.001},{"name":"regularization","type":"double","data":1.0}])"}});
    change("PutDefinition", {{"id", Quote(id)},
                             {"meta_json", Quote(json::Encode(meta))},
                             {"card_json", Quote(definition)}});
    std::string output_schema = "table Data { value:double; }\ntable Outputs {";
    if (outputs.empty()) outputs.insert("data");
    for (const auto& name : outputs) output_schema += " " + name + ":Data;";
    output_schema += " }\nroot_type Outputs;\n";
    change("PutSource",
           {{"path", Quote("definitions/" + id + "/src/main.py")},
            {"bytes", Quote("def run(data, report):\n    return data\n")}});
    change("PutSource", {{"path", Quote("definitions/" + id + "/_outputs.fbs")},
                         {"bytes", Quote(output_schema)}});
  }
  ASSERT_GT(drafts, 1u);
  // These are the actual bytes of a bundled Draft read, not malformed model
  // input. The old 512-byte event summary cuts its Japanese text mid-character.
  const auto read_arguments = R"({"path":"definitions/boosting/_meta.json"})";
  const auto read =
      Tool(conversation, "project_file_read", read_arguments, "read-draft");
  ASSERT_TRUE(read.Code().empty());
  ASSERT_FALSE(JsonBool(read.View(), "/isError")) << read.View();
  if (std::get<0>(GetParam()) == "ja") {
    schema::v::String summary;
    summary.value() = JsonString(read.View(), "/text").substr(0, 512);
    EXPECT_THROW(schema::encode(summary), std::runtime_error);
  }
  ModelServer model;
  Configure(model);
  model.Queue(AnthropicTool("inspect", "project_inspect", "{}"));
  model.Queue(AnthropicTool("read-draft", "project_file_read", read_arguments));
  model.Queue(AnthropicTool(
      "implement", "project_save",
      json::Encode(json::Members{{"base_rev", json::Get(snapshot, "rev")},
                                 {"changes", json::Encode(changes)}})));
  model.Queue(AnthropicText("Generated Cards."));
  ASSERT_TRUE(Send(conversation, "Generate Cards: implement the Draft Cards.")
                  .Code()
                  .empty());
  const auto state = WaitUntilIdle(conversation);
  EXPECT_NE(state.find("Generated Cards."), std::string::npos) << state;
  EXPECT_EQ(state.find("JSON encoding failed"), std::string::npos) << state;
  ASSERT_TRUE(model.WaitForRequests(4));
  const auto request = model.Requests().at(3);
  const auto result = JsonString(request, "/messages/6/content/0/content");
  EXPECT_FALSE(JsonBool(request, "/messages/6/content/0/is_error")) << result;
  EXPECT_NE(result.find("SaveResult"), std::string::npos) << request;
  const auto after = json::Object(
      json::Get(json::Object(JsonString(Inspect(conversation).View(), "/text")),
                "value"));
  EXPECT_EQ(json::UInt(
                json::String(json::Get(after, "rev"), json::Get(after, "rev"))),
            json::UInt(json::String(json::Get(snapshot, "rev"),
                                    json::Get(snapshot, "rev"))) +
                1);
  const auto layout = json::Object(Read(root / "layout.json"));
  EXPECT_TRUE(json::Object(layout.at("cards")).contains("report"));
  for (const auto& raw : cards) {
    const auto id = json::Text(json::Object(raw), "id");
    if (id == "report" ||
        json::Text(json::Object(Read(root / "definitions" / id / "_card.json")),
                   "template") == "code")
      EXPECT_NE(Read(root / "definitions" / id / "_card.json").find("0.001"),
                std::string::npos);
  }
  const auto geometry = Call(
      *host, H::RequestPayload::DisplayGeometryRequest,
      [](Builder& b) { return H::CreateDisplayGeometryRequest(b).Union(); });
  ASSERT_TRUE(geometry.Code().empty());
  const auto* value = geometry.HostValue()->value_as_DisplayGeometries();
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(value->top_to_bottom()->width(), 320);
  EXPECT_EQ(value->left_to_right()->height(), 170);
}
INSTANTIATE_TEST_SUITE_P(BundledDemo, MnistSaveFixture,
                         testing::Combine(testing::Values(std::string("ja"),
                                                          std::string("en")),
                                          testing::Bool()));
}  // namespace
}  // namespace tsugumi::test
