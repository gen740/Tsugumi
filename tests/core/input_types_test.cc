#include "host/input_types.hh"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>

#include "agent/catalog.hh"
#include "agent/context.hh"
#include "input.hh"
#include "tsugumi/document/input_types.hh"
#include "tsugumi/util/crypto.hh"
#include "wire.hh"

namespace tsugumi {
namespace {
using Files = std::map<std::string, std::string>;
const std::string kSchema = R"(namespace sample;
table Child { item_count:int; }
table Message { noise_sd:double; title:string; children:[Child]; values:[float]; }
table Outputs { out:Message; } root_type Outputs;)";
auto Graph(const std::string& cards, const std::string& wires, Files files)
    -> std::shared_ptr<const Resolution> {
  Document doc;
  doc.project = parse_project_json(
      "{\"pkgsPath\":\"tsugumi:default\",\"canvas\":{\"cards\":" + cards +
      ",\"connections\":" + wires + "}}");
  for (const auto& [path, text] : files) {
    doc.sources[path] = {crypto::sha256_hex(text), text.size()};
    if (!path.starts_with("definitions/")) continue;
    const auto end = path.find('/', 12);
    const auto id = path.substr(12, end - 12);
    if (path.ends_with("/_card.json")) {
      doc.definitions[id].card = parse_card_json(id, text);
      doc.definitions[id].card_json = text;
    }
  }
  return resolve(doc, [files](const auto& path) -> std::optional<std::string> {
    const auto found = files.find(path);
    return found == files.end() ? std::nullopt : std::optional{found->second};
  });
}
auto Present(const Resolution& graph, const CardPath& card,
             const std::optional<std::string>& language = {},
             const std::optional<std::string>& output = {})
    -> Tsugumi::Host::InputTypesT {
  flatbuffers::FlatBufferBuilder b;
  std::vector<flatbuffers::Offset<Tsugumi::RPC::InputType>> ends;
  for (const auto& end : input_types(graph, card, output))
    ends.push_back(Tsugumi::RPC::CreateInputType(
        b, b.CreateString(end.port),
        end.source ? wire::path(b, end.source->path) : 0,
        end.source ? b.CreateString(end.source_port) : 0,
        end.type ? wire::port(b, *end.type) : 0));
  const auto result = Tsugumi::RPC::CreateInputTypesResult(
      b, 3, b.CreateString(graph.find(card)->settings.language),
      b.CreateVector(ends));
  b.Finish(result);
  return host::InputTypes(*flatbuffers::GetRoot<Tsugumi::RPC::InputTypesResult>(
                              b.GetBufferPointer()),
                          language);
}
TEST(InputTypes, PlainWireDefaultLanguageFileAndNoSource) {
  const auto graph = Graph(
      R"([{"id":"src"},{"id":"dest"},{"id":"file"}])",
      R"([{"from":{"card":"src","port":"out"},"to":{"card":"dest","port":"data"}},
          {"from":{"card":"file","port":"out"},"to":{"card":"dest","port":"file"}}])",
      {{"definitions/src/_card.json", "{}"},
       {"definitions/src/_outputs.fbs", kSchema},
       {"definitions/file/_card.json", R"({"template":"input"})"},
       {"definitions/file/_outputs.fbs",
        R"(attribute "tsugumi"; table File { value:string; } table Outputs { out:File (tsugumi:"file"); } root_type Outputs;)"},
       {"definitions/dest/_card.json",
        R"({"language":"python","inputs":[{"name":"data"},{"name":"file"},{"name":"missing","optional":true}]})"}});
  const auto result = Present(*graph, {"dest"});
  ASSERT_EQ(result.inputs.size(), 3u);
  EXPECT_EQ(result.language, "python");
  EXPECT_EQ(result.inputs[0]->source_card->segments, (CardPath{"src"}));
  EXPECT_EQ(result.inputs[0]->source_port, "out");
  EXPECT_EQ(result.inputs[0]->kind, "value");
  EXPECT_NE(result.inputs[0]->definition.find("noiseSd: float"),
            std::string::npos);
  EXPECT_NE(result.inputs[0]->definition.find("title: Optional[bytes]"),
            std::string::npos);
  EXPECT_EQ(result.inputs[1]->kind, "file");
  EXPECT_NE(result.inputs[1]->definition.find("value: Optional[bytes]"),
            std::string::npos);
  EXPECT_EQ(result.inputs[2]->state, "no_source");
  EXPECT_FALSE(result.inputs[2]->source_card);
  EXPECT_TRUE(result.inputs[2]->definition.empty());
  const auto upstream = Present(*graph, {"src"}, "cpp", "out");
  ASSERT_EQ(upstream.inputs.size(), 1u);
  EXPECT_NE(upstream.inputs[0]->definition.find("double noise_sd"),
            std::string::npos);
}
TEST(InputTypes, DistinctUnnamedDraftEndsKeepTheirSourceTypes) {
  const auto graph = Graph(
      R"([{"id":"src"},{"id":"draft"},{"id":"other"}])",
      R"([{"from":{"card":"src","port":"out"},"to":{"card":"draft","port":""}},
          {"from":{"card":"other","port":"out"},"to":{"card":"draft","port":""}}])",
      {{"definitions/src/_card.json", "{}"},
       {"definitions/src/_outputs.fbs", kSchema},
       {"definitions/other/_card.json", R"({"template":"trigger"})"}});
  const auto ends = input_types(*graph, {"draft"});
  ASSERT_EQ(ends.size(), 2u);
  CardPath sources;
  for (const auto& end : ends) {
    EXPECT_TRUE(end.port.empty());
    ASSERT_TRUE(end.source && end.type);
    EXPECT_EQ(end.source_port, "out");
    EXPECT_EQ(end.type->type, end.source->output("out")->type);
    sources.push_back(end.source->path.front());
  }
  std::ranges::sort(sources);
  EXPECT_EQ(sources, (CardPath{"other", "src"}));
  EXPECT_EQ(Present(*graph, {"draft"}, "python").inputs.size(), 2u);
}
TEST(InputTypes, NestedBoundariesShareTheExecutableEndpointResolution) {
  const auto graph = Graph(
      R"([{"id":"src"},{"id":"container"},{"id":"dest"}])",
      R"([{"from":{"card":"src","port":"out"},"to":{"card":"container","port":"in"}},
          {"from":{"card":"container","port":"out"},"to":{"card":"dest","port":"data"}}])",
      {{"definitions/src/_card.json", "{}"},
       {"definitions/src/_outputs.fbs", kSchema},
       {"definitions/dest/_card.json", R"({"inputs":[{"name":"data"}]})"},
       {"definitions/container/_card.json", R"({"external":"outer"})"},
       {"externals/outer/tsugumi.json",
        R"({"canvas":{"cards":[{"id":"inner"}],"connections":[]},"boundary":{"inputs":{"in":{"card":"inner","port":"in"}},"outputs":{"out":{"card":"inner","port":"out"}}}})"},
       {"externals/outer/definitions/inner/_card.json",
        R"({"external":"nested"})"},
       {"externals/outer/externals/nested/tsugumi.json",
        R"({"canvas":{"cards":[{"id":"leaf"}],"connections":[]},"boundary":{"inputs":{"in":{"card":"leaf","port":"in"}},"outputs":{"out":{"card":"leaf","port":"out"}}}})"},
       {"externals/outer/externals/nested/definitions/leaf/_card.json",
        R"({"inputs":[{"name":"in"}]})"},
       {"externals/outer/externals/nested/definitions/leaf/_outputs.fbs",
        kSchema}});
  const auto ends = input_types(*graph, {"dest"});
  ASSERT_EQ(ends.size(), 1u);
  ASSERT_NE(ends[0].type, nullptr);
  EXPECT_EQ(ends[0].source->path, (CardPath{"container", "inner", "leaf"}));
  ASSERT_TRUE(ends[0].type->type);
  EXPECT_TRUE(types::compatible(*ends[0].type->type,
                                *graph->find({"src"})->output("out")->type));
  const auto boundary = input_types(*graph, {"container"}, "out");
  ASSERT_EQ(boundary.size(), 1u);
  EXPECT_EQ(boundary[0].type->type, ends[0].type->type);
  const auto inputs = input_types(*graph, {"container"});
  ASSERT_EQ(inputs.size(), 1u);
  EXPECT_EQ(inputs[0].source->path, (CardPath{"src"}));
  EXPECT_EQ(inputs[0].port, "in");
}
TEST(InputTypes, CollectMergeBranchAndSwitchUseTheResolverTypes) {
  const auto graph = Graph(
      R"([{"id":"src"},{"id":"collect"},{"id":"merge"},{"id":"branch"},{"id":"switch"}])",
      R"([{"from":{"card":"src","port":"out"},"to":{"card":"collect","port":"in"}},
          {"from":{"card":"collect","port":"out"},"to":{"card":"merge","port":"items"}},
          {"from":{"card":"merge","port":"out"},"to":{"card":"branch","port":"value"}},
          {"from":{"card":"switch","port":"value"},"to":{"card":"branch","port":"condition"}}])",
      {{"definitions/src/_card.json", "{}"},
       {"definitions/src/_outputs.fbs", kSchema},
       {"definitions/collect/_card.json", R"({"template":"collect"})"},
       {"definitions/merge/_card.json",
        R"({"template":"merge","inputs":[{"name":"items"}]})"},
       {"definitions/branch/_card.json", R"({"template":"branch"})"},
       {"definitions/switch/_card.json", R"({"template":"switch"})"}});
  for (const auto& [card, port] :
       std::vector<std::pair<std::string, std::string>>{{"collect", "out"},
                                                        {"merge", "out"},
                                                        {"branch", "a"},
                                                        {"switch", "value"}}) {
    const auto ends = input_types(*graph, {card}, port);
    ASSERT_EQ(ends.size(), 1u);
    ASSERT_TRUE(ends[0].type->type) << card;
    EXPECT_EQ(ends[0].type->type, graph->find({card})->output(port)->type);
    EXPECT_FALSE(
        Present(*graph, {card}, "python", port).inputs[0]->definition.empty());
  }
}
TEST(InputTypes, CompatibleProducersUseTheDestinationCodeContract) {
  const auto graph = Graph(
      R"([{"id":"first"},{"id":"second"},{"id":"dest"}])",
      R"([{"from":{"card":"first","port":"out"},"to":{"card":"dest","port":"in"}},
          {"from":{"card":"second","port":"out"},"to":{"card":"dest","port":"in"}}])",
      {{"definitions/first/_card.json", "{}"},
       {"definitions/first/_outputs.fbs",
        "table First { value:int; } table Outputs { out:First; } root_type "
        "Outputs;"},
       {"definitions/second/_card.json", "{}"},
       {"definitions/second/_outputs.fbs",
        "table Second { value:int; } table Outputs { out:Second; } root_type "
        "Outputs;"},
       {"definitions/dest/_card.json", R"({"inputs":[{"name":"in"}]})"}});
  const auto ends = input_types(*graph, {"dest"});
  ASSERT_EQ(ends.size(), 2u);
  EXPECT_EQ(ends[0].type, graph->find({"dest"})->input("in"));
  EXPECT_EQ(ends[1].type, ends[0].type);
  const auto descriptions = Present(*graph, {"dest"}, "python");
  EXPECT_EQ(descriptions.inputs[0]->definition,
            descriptions.inputs[1]->definition);
}
TEST(InputTypes, LargeDefinitionAndModelResultAreNeverClipped) {
  std::string schema = "table Large {\n";
  for (int i = 0; i < 5000; ++i)
    schema += "  field_" + std::to_string(i) + ":double;\n";
  schema += "} root_type Large;\n";
  const auto definition = sdk::DescribeInput(schema, "python");
  ASSERT_GT(definition.size(), agent::kToolResultBytes);
  EXPECT_NE(definition.find("field4999: float"), std::string::npos);
  const auto graph = Graph(
      R"([{"id":"src"},{"id":"draft"}])",
      R"([{"from":{"card":"src","port":"out"},"to":{"card":"draft","port":""}}])",
      {{"definitions/src/_card.json", "{}"},
       {"definitions/src/_outputs.fbs",
        schema.substr(0, schema.find("root_type")) +
            "table Outputs { out:Large; } root_type Outputs;"}});
  const auto presented = Present(*graph, {"draft"}, "python");
  ASSERT_EQ(presented.inputs.size(), 1u);
  EXPECT_EQ(presented.inputs[0]->definition, definition);
  agent::ContentBlock call;
  call.type = "tool_use";
  call.name = "input_types";
  call.id = "types";
  agent::ContentBlock result;
  result.type = "tool_result";
  result.id = call.id;
  result.text = definition;
  const std::vector<agent::Message> messages{
      {"assistant", {call}, {}, {}, agent::ProviderAPI::AnthropicMessages},
      {"user", {result}, {}, {}, agent::ProviderAPI::AnthropicMessages}};
  EXPECT_EQ(agent::RequestMessages(messages, 0, 100).back().content[0].text,
            definition);
  EXPECT_EQ(sdk::DescribeInput(schema, "unknown"), schema);
}
}  // namespace
}  // namespace tsugumi

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "--describe") {
    std::ifstream file(argv[3]);
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    std::cout << tsugumi::sdk::DescribeInput(schema, argv[2]);
    return 0;
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
