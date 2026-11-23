#include "host/input_types.hh"

#include <gtest/gtest.h>

#include <algorithm>

#include "agent/catalog.hh"
#include "agent/context.hh"
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
  doc.project_buffer =
      parse_project_json("{\"nixPath\":[],\"canvas\":{\"cards\":" + cards +
                         ",\"connections\":" + wires + "}}")
          .buffer;
  for (const auto& [path, text] : files) {
    doc.sources[path] = {crypto::sha256_hex(text), text.size()};
    if (!path.starts_with("definitions/")) continue;
    const auto end = path.find('/', 12);
    const auto id = path.substr(12, end - 12);
    if (path.ends_with("/_card.json")) {
      doc.definitions[id].card_buffer = parse_card_json(id, text).buffer;
      doc.definitions[id].card_json = text;
    }
  }
  return resolve(doc, [files](const auto& path) -> std::optional<std::string> {
    const auto found = files.find(path);
    return found == files.end() ? std::nullopt : std::optional{found->second};
  });
}
auto Present(const Resolution& graph, const CardPath& card,
             const std::optional<std::string>& output = {})
    -> Tsugumi::Host::InputTypesT {
  flatbuffers::FlatBufferBuilder b;
  std::vector<flatbuffers::Offset<Tsugumi::RPC::InputType>> ends;
  for (const auto& end : input_types(graph, card, output))
    ends.push_back(Tsugumi::RPC::CreateInputType(
        b, b.CreateString(end.port),
        end.source ? wire::path(b, end.source->path) : 0,
        end.source ? b.CreateString(end.source_port) : 0,
        end.type ? wire::resolved_port(b, *end.type) : 0));
  const auto result =
      Tsugumi::RPC::CreateInputTypesResult(b, 3, b.CreateVector(ends));
  b.Finish(result);
  return host::InputTypes(*flatbuffers::GetRoot<Tsugumi::RPC::InputTypesResult>(
      b.GetBufferPointer()));
}
TEST(InputTypes, PlainWireSchemasAndRouting) {
  const auto graph = Graph(
      R"([{"id":"src"},{"id":"dest"},{"id":"file"}])",
      R"([{"from":{"card":"src","port":"out"},"to":{"card":"dest","port":"data"}},
          {"from":{"card":"file","port":"out"},"to":{"card":"dest","port":"file"}}])",
      {{"definitions/src/_card.json", R"({"type":"code"})"},
       {"definitions/src/_outputs.fbs", kSchema},
       {"definitions/file/_card.json", R"({"type":"input"})"},
       {"definitions/file/_outputs.fbs",
        kSchema},
       {"definitions/dest/_card.json",
        R"({"type":"code","inputs":[{"name":"data"},{"name":"file"},{"name":"missing"}]})"}});
  const auto result = Present(*graph, {"dest"});
  ASSERT_EQ(result.inputs.size(), 3u);
  EXPECT_EQ(result.rev, 3u);
  EXPECT_EQ(result.inputs[0]->port, "data");
  EXPECT_EQ(result.inputs[0]->state, "resolved");
  EXPECT_EQ(result.inputs[0]->source_card->segments, (CardPath{"src"}));
  EXPECT_EQ(result.inputs[0]->source_port, "out");
  EXPECT_EQ(result.inputs[0]->kind, "value");
  EXPECT_NE(result.inputs[0]->definition.find("noise_sd:double"),
            std::string::npos);
  EXPECT_NE(result.inputs[0]->definition.find("title:string"),
            std::string::npos);
  std::string root;
  std::string error;
  EXPECT_TRUE(types::schema_bfbs(result.inputs[0]->definition, &root, &error))
      << error;
  EXPECT_EQ(root, "inputs.data.sample.Message");
  EXPECT_EQ(result.inputs[1]->kind, "value");
  EXPECT_EQ(result.inputs[1]->state, "resolved");
  EXPECT_FALSE(result.inputs[1]->definition.empty());
  EXPECT_EQ(result.inputs[2]->state, "no_source");
  EXPECT_FALSE(result.inputs[2]->source_card);
  EXPECT_TRUE(result.inputs[2]->definition.empty());
  const auto upstream = Present(*graph, {"src"}, "out");
  ASSERT_EQ(upstream.inputs.size(), 1u);
  EXPECT_NE(upstream.inputs[0]->definition.find("noise_sd:double"),
            std::string::npos);
}
TEST(InputTypes, DistinctUnnamedDraftEndsKeepTheirSourceTypes) {
  const auto graph = Graph(
      R"([{"id":"src"},{"id":"draft"},{"id":"other"}])",
      R"([{"from":{"card":"src","port":"out"},"to":{"card":"draft","port":""}},
          {"from":{"card":"other","port":"out"},"to":{"card":"draft","port":""}}])",
      {{"definitions/src/_card.json", R"({"type":"code"})"},
       {"definitions/src/_outputs.fbs", kSchema},
       {"definitions/other/_card.json", R"({"type":"trigger"})"}});
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
  EXPECT_EQ(Present(*graph, {"draft"}).inputs.size(), 2u);
}
TEST(InputTypes, NestedBoundariesShareTheExecutableEndpointResolution) {
  const auto graph = Graph(
      R"([{"id":"src"},{"id":"container"},{"id":"dest"}])",
      R"([{"from":{"card":"src","port":"out"},"to":{"card":"container","port":"in"}},
          {"from":{"card":"container","port":"out"},"to":{"card":"dest","port":"data"}}])",
      {{"definitions/src/_card.json", R"({"type":"code"})"},
       {"definitions/src/_outputs.fbs", kSchema},
       {"definitions/dest/_card.json",
        R"({"type":"code","inputs":[{"name":"data"}]})"},
       {"definitions/container/_card.json",
        R"({"type":"subgraph","external":"outer"})"},
       {"externals/outer/tsugumi.json",
        R"({"canvas":{"cards":[{"id":"inner"}],"connections":[]},"boundary":{"inputs":[{"name":"in","card":"inner","port":"in"}],"outputs":[{"name":"out","card":"inner","port":"out"}]}})"},
       {"externals/outer/definitions/inner/_card.json",
        R"({"type":"subgraph","external":"nested"})"},
       {"externals/outer/externals/nested/tsugumi.json",
        R"({"canvas":{"cards":[{"id":"leaf"}],"connections":[]},"boundary":{"inputs":[{"name":"in","card":"leaf","port":"in"}],"outputs":[{"name":"out","card":"leaf","port":"out"}]}})"},
       {"externals/outer/externals/nested/definitions/leaf/_card.json",
        R"({"type":"code","inputs":[{"name":"in"}]})"},
       {"externals/outer/externals/nested/definitions/leaf/_outputs.fbs",
        kSchema}});
  const auto ends = input_types(*graph, {"dest"});
  ASSERT_EQ(ends.size(), 1u);
  ASSERT_NE(ends[0].type, nullptr);
  EXPECT_EQ(ends[0].source->path, (CardPath{"container", "inner", "leaf"}));
  ASSERT_TRUE(ends[0].type->type);
  EXPECT_EQ(ends[0].type->type, ends[0].source->output("out")->type);
  const auto boundary = input_types(*graph, {"container"}, "out");
  ASSERT_EQ(boundary.size(), 1u);
  EXPECT_EQ(boundary[0].type->type, ends[0].type->type);
  const auto inputs = input_types(*graph, {"container"});
  ASSERT_EQ(inputs.size(), 1u);
  EXPECT_EQ(inputs[0].source->path, (CardPath{"src"}));
  EXPECT_EQ(inputs[0].port, "in");
  ASSERT_NE(inputs[0].type, nullptr);
  EXPECT_EQ(inputs[0].type->type, inputs[0].source->output("out")->type);
}
TEST(InputTypes, CollectMergeBranchAndInputUseTheResolverTypes) {
  const auto graph = Graph(
      R"([{"id":"src"},{"id":"collect"},{"id":"merge"},{"id":"branch"},{"id":"condition"}])",
      R"([{"from":{"card":"src","port":"out"},"to":{"card":"collect","port":"in"}},
          {"from":{"card":"collect","port":"out"},"to":{"card":"merge","port":"items"}},
          {"from":{"card":"merge","port":"out"},"to":{"card":"branch","port":"value"}},
          {"from":{"card":"condition","port":"value"},"to":{"card":"branch","port":"condition"}}])",
      {{"definitions/src/_card.json", R"({"type":"code"})"},
       {"definitions/src/_outputs.fbs", kSchema},
       {"definitions/collect/_card.json", R"({"type":"collect"})"},
       {"definitions/merge/_card.json",
        R"({"type":"merge","inputs":[{"name":"items"}]})"},
       {"definitions/branch/_card.json", R"({"type":"branch"})"},
       {"definitions/condition/_card.json", R"({"type":"input"})"},
       {"definitions/condition/_outputs.fbs",
        "table Condition { value:bool; } table Outputs { value:Condition; } "
        "root_type Outputs;"}});
  for (const auto& [card, port] :
       std::vector<std::pair<std::string, std::string>>{
           {"collect", "out"},
           {"merge", "out"},
           {"branch", "a"},
           {"condition", "value"}}) {
    const auto ends = input_types(*graph, {card}, port);
    ASSERT_EQ(ends.size(), 1u);
    ASSERT_TRUE(ends[0].type->type) << card;
    EXPECT_EQ(ends[0].type->type, graph->find({card})->output(port)->type);
    EXPECT_FALSE(Present(*graph, {card}, port).inputs[0]->definition.empty());
  }
}
TEST(InputTypes, SecondWireLeavesOnlyTheFirstProducer) {
  const auto graph = Graph(
      R"([{"id":"first"},{"id":"second"},{"id":"dest"}])",
      R"([{"from":{"card":"first","port":"out"},"to":{"card":"dest","port":"in"}},
          {"from":{"card":"second","port":"out"},"to":{"card":"dest","port":"in"}}])",
      {{"definitions/first/_card.json", R"({"type":"code"})"},
       {"definitions/first/_outputs.fbs",
        "table First { value:int; } table Outputs { out:First; } root_type "
        "Outputs;"},
       {"definitions/second/_card.json", R"({"type":"code"})"},
       {"definitions/second/_outputs.fbs",
        "table Second { value:int; } table Outputs { out:Second; } root_type "
        "Outputs;"},
       {"definitions/dest/_card.json",
        R"({"type":"code","inputs":[{"name":"in"}]})"}});
  const auto ends = input_types(*graph, {"dest"});
  ASSERT_EQ(ends.size(), 1u);
  EXPECT_EQ(ends[0].source->path, (CardPath{"first"}));
  EXPECT_EQ(ends[0].type, graph->find({"dest"})->input("in"));
  EXPECT_EQ(ends[0].type->type, graph->find({"first"})->output("out")->type);
}
TEST(InputTypes, MergeBoxesOpaqueBytesAndRendersNestedTypes) {
  std::string error;
  const auto first = types::compile("table Value { x:int; }", "Value", &error);
  const auto second =
      types::compile("table Value { x:long; }", "Value", &error);
  ASSERT_TRUE(first && second) << error;
  const types::PortType a{"a", true, *first};
  const types::PortType b{"b", false, *second};
  const types::PortType c{"c", false, *first};
  const types::PortType unconnected{"unused",                                     false, nullptr};
  const auto same = types::merged_type({&a, &c}, &error);
  ASSERT_TRUE(same) << error;
  EXPECT_EQ(same->type, *first);
  EXPECT_TRUE(same->tags.empty());

  const auto mixed = types::merged_type({&a, &b, &c}, &error);
  ASSERT_TRUE(mixed) << error;
  EXPECT_EQ(mixed->tags.at("a"), mixed->tags.at("c"));
  EXPECT_NE(mixed->tags.at("a"), mixed->tags.at("b"));
  const types::PortType merged{"merged", false,
                               mixed->type};
  const auto catalog =
      types::card_catalog({a, merged, unconnected}, /*inputs=*/true);
  std::string root;
  ASSERT_TRUE(types::schema_bfbs(catalog, &root, &error)) << error;
  EXPECT_EQ(root, "inputs.Inputs");
  EXPECT_NE(catalog.find("table ValueBox2"), std::string::npos);
  EXPECT_NE(catalog.find(
                "nested_flatbuffer: \"inputs.merged.tsugumi.merge.t1.Value\""),
            std::string::npos);
  EXPECT_EQ(catalog.find("unused:"), std::string::npos);
  EXPECT_EQ(catalog.find("required"), std::string::npos);

  const std::string bytes{"\xff\x00\xf1", 3};
  const auto wrapped = types::wrap_in_union(mixed->tags.at("b"), bytes);
  const auto* value = flatbuffers::GetRoot<flatbuffers::Table>(wrapped.data());
  EXPECT_EQ(value->GetField<uint8_t>(4, 0), mixed->tags.at("b"));
  const auto* box = value->GetPointer<const flatbuffers::Table*>(6);
  ASSERT_NE(box, nullptr);
  const auto* nested = box->GetPointer<const flatbuffers::Vector<uint8_t>*>(4);
  ASSERT_NE(nested, nullptr);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(nested->data()),
                             nested->size()),
            bytes);
}

TEST(InputTypes, CatalogKeepsDeclarationOrderAndNestedBufferReferences) {
  const auto parsed = types::parse_catalog(
      R"(namespace Other;
table Outputs { unrelated:string; }
namespace Sample;
table Inner { value:long; }
table Outer { raw:[ubyte] (nested_flatbuffer: "Inner"); }
table Outputs { first:Outer (id: 1); second:Inner (id: 0); }
root_type Outputs;)",
      "_outputs.fbs");
  ASSERT_TRUE(parsed.errors.empty());
  ASSERT_EQ(parsed.ports.size(), 2u);
  EXPECT_EQ(parsed.ports[0].name, "first");
  EXPECT_EQ(parsed.ports[1].name, "second");
  const auto catalog = types::card_catalog(parsed.ports, /*inputs=*/false);
  EXPECT_NE(catalog.find("first:outputs.first.Sample.Outer (id: 0)"),
            std::string::npos);
  EXPECT_NE(catalog.find("second:outputs.second.Sample.Inner (id: 1)"),
            std::string::npos);
  EXPECT_NE(catalog.find("nested_flatbuffer: \"outputs.first.Sample.Inner\""),
            std::string::npos);
  std::string root;
  std::string error;
  EXPECT_TRUE(types::schema_bfbs(catalog, &root, &error)) << error;
}

TEST(InputTypes, LargeDefinitionAndModelResultAreNeverClipped) {
  std::string schema = "table Large {\n";
  for (int i = 0; i < 5000; ++i)
    schema += "  field_" + std::to_string(i) + ":double;\n";
  schema += "}\n";
  const auto graph = Graph(
      R"([{"id":"src"},{"id":"draft"}])",
      R"([{"from":{"card":"src","port":"out"},"to":{"card":"draft","port":""}}])",
      {{"definitions/src/_card.json", R"({"type":"code"})"},
       {"definitions/src/_outputs.fbs",
        schema + "table Outputs { out:Large; } root_type Outputs;"}});
  const auto presented = Present(*graph, {"draft"});
  ASSERT_EQ(presented.inputs.size(), 1u);
  const auto& definition = presented.inputs[0]->definition;
  ASSERT_GT(definition.size(), agent::kToolResultBytes);
  EXPECT_NE(definition.find("field_4999:double"), std::string::npos);
  EXPECT_NE(definition.find("root_type Large;"), std::string::npos);
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
}
}  // namespace
}  // namespace tsugumi
