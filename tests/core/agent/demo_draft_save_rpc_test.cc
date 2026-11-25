#include <algorithm>
#include <set>

#include "project_fixture.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::test {
namespace {
namespace json = ::tsugumi::json;

TEST_F(AgentProjectFixture,
       ImplementsJapaneseBundledDraftsWithoutWritingLayout) {
  const auto demo = std::filesystem::path(TSUGUMI_GUIDE_TEST_ROOT) / "demos" /
                    "ja" / "gw150914-draft";
  const auto root = directory.path / "project";
  // Bundled fixtures in the Nix store are read-only; the opened project is a
  // writable copy, as it is when the app opens a demo.
  for (const auto& file : std::filesystem::recursive_directory_iterator(demo))
    if (file.is_regular_file())
      Write(root / std::filesystem::relative(file.path(), demo),
            Read(file.path()));
  const auto document = Read(root / "tsugumi.json");
  StartProject(false, document, "{}");
  const auto saved_layout = Read(root / "layout.bin");
  const auto conversation = Conversation("gw150914-generate-cards");
  Grant(conversation);
  Grant(conversation, Tsugumi::Service::Access::Write);
  const auto snapshot = json::Object(
      json::Get(json::Object(JsonString(Inspect(conversation).View(), "/text")),
                "value"));
  const auto sources = json::Array(snapshot.at("sources"));
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
  std::vector<std::string> implemented;
  for (const auto& raw : cards) {
    const auto id = json::Text(json::Object(raw), "id");
    if (id != "report" &&
        json::Text(json::Object(Read(root / "definitions" / id / "_card.json")),
                   "type") != "draft")
      continue;
    implemented.push_back(id);
    const auto readme = id == "report"
                            ? "---\ntitle: report\ndescription: Compare the "
                              "strain analyses\n---\n\nPresent the comparison."
                            : Read(root / "definitions" / id / "README.md");
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
    const auto definition = FixtureCodeCard(
        json::Encode(json::Members{{"inputs", json::Encode(input_defs)}}));
    change("PutDefinition", {{"id", Quote(id)},
                             {"readme", Quote(readme)},
                             {"card_json", Quote(definition)}});
    std::string output_schema = "table Data { value:double; }\ntable Outputs {";
    if (outputs.empty()) outputs.insert("data");
    for (const auto& name : outputs) output_schema += " " + name + ":Data;";
    output_schema += " }\nroot_type Outputs;\n";
    change("PutSource",
           {{"path", Quote("definitions/" + id + "/src/main.py")},
            {"bytes", Quote("def main(inputs, report):\n    return {}\n")}});
    change("PutSource", {{"path", Quote("definitions/" + id + "/_outputs.fbs")},
                         {"bytes", Quote(output_schema)}});
    const auto params_path = "definitions/" + id + "/params.nix";
    json::Members params{
        {"path", Quote(params_path)},
        {"bytes", Quote("{ rate = 0.001; regularization = 1.0; }")}};
    // The bundled Drafts already have params.nix. Replacing one needs its
    // snapshot hash; the new report Card creates the file without a hash.
    for (const auto& raw_source : sources) {
      const auto source = json::Object(raw_source);
      if (json::Text(source, "path") == params_path) {
        params["base_hash"] = source.at("hash");
        break;
      }
    }
    change("PutSource", std::move(params));
  }
  // The bundled Japanese text exercises multibyte clipping in tool events.
  const auto read_arguments = R"({"path":"definitions/load/README.md"})";
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
  WaitUntilIdle(conversation);
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 4u);
  ASSERT_FALSE(JsonBool(requests.back(), "/messages/6/content/0/is_error"))
      << JsonString(requests.back(), "/messages/6/content/0/content");
  const auto after = json::Object(
      json::Get(json::Object(JsonString(Inspect(conversation).View(), "/text")),
                "value"));
  EXPECT_EQ(json::UInt(
                json::String(json::Get(after, "rev"), json::Get(after, "rev"))),
            json::UInt(json::String(json::Get(snapshot, "rev"),
                                    json::Get(snapshot, "rev"))) +
                1);
  EXPECT_EQ(Read(root / "layout.bin"), saved_layout);
  const auto saved =
      json::Object(json::Object(Read(root / "tsugumi.json")).at("canvas"));
  const auto placed = json::Array(saved.at("cards"));
  EXPECT_TRUE(std::any_of(placed.begin(), placed.end(), [&](const auto& entry) {
    return json::Text(json::Object(entry), "id") == "report";
  }));
  for (const auto& id : implemented) {
    SCOPED_TRACE(id);
    const auto definition = Read(root / "definitions" / id / "_card.json");
    EXPECT_EQ(JsonString(definition, "/type"), "code");
    EXPECT_EQ(Read(root / "definitions" / id / "params.nix"),
              "{ rate = 0.001; regularization = 1.0; }");
  }
}
}  // namespace
}  // namespace tsugumi::test
