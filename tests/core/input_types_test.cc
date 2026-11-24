#include "host/input_types.hh"

#include <gtest/gtest.h>

#include <algorithm>

#include "agent/catalog.hh"
#include "agent/context.hh"
#include "test_document.hh"
#include "tsugumi/document/input_schema.hh"
#include "tsugumi/document/input_types.hh"
#include "tsugumi/document/schema_id.hh"
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
      parse_project_json(test::project_json("{\"canvas\":{\"cards\":" + cards +
                                            ",\"connections\":" + wires + "}}"))
          .buffer;
  for (const auto& [path, text] : files) {
    if (Document::is_external_document_file(path))
      doc.sources[path] = external_document_source(path, text);
    else
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
      {{"definitions/src/_card.json", test::card_json(R"({"type":"code"})")},
       {"definitions/src/_outputs.fbs", kSchema},
       {"definitions/file/_card.json", test::card_json(R"({"type":"input"})")},
       {"definitions/file/_outputs.fbs",
        R"(table Message { body:string; } table Outputs { out:Message; } root_type Outputs;)"},
       {"definitions/dest/_card.json",
        test::card_json(
            R"({"type":"code","inputs":[{"name":"data"},{"name":"file"},{"name":"missing"}]})")}});
  const auto result = Present(*graph, {"dest"});
  ASSERT_EQ(result.inputs.size(), 3u);
  EXPECT_EQ(result.rev, 3u);
  EXPECT_EQ(result.inputs[0]->port, "data");
  EXPECT_EQ(result.inputs[0]->state, "resolved");
  EXPECT_EQ(result.inputs[0]->source_card->segments, (CardPath{"src"}));
  EXPECT_EQ(result.inputs[0]->source_port, "out");
  EXPECT_NE(result.inputs[0]->definition.find("noise_sd:double"),
            std::string::npos);
  EXPECT_NE(result.inputs[0]->definition.find("title:string"),
            std::string::npos);
  std::string root;
  std::string error;
  EXPECT_TRUE(types::schema_bfbs(result.inputs[0]->definition, &root, &error))
      << error;
  EXPECT_EQ(root, "inputs.data.sample.Message");
  EXPECT_EQ(result.inputs[1]->state, "resolved");
  EXPECT_NE(result.inputs[1]->definition.find("body:string"),
            std::string::npos);
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
      R"([{"from":{"card":"src","port":"out"},"to":{"card":"draft"}},
          {"from":{"card":"other","port":"out"},"to":{"card":"draft"}}])",
      {{"definitions/src/_card.json", test::card_json(R"({"type":"code"})")},
       {"definitions/src/_outputs.fbs", kSchema},
       {"definitions/other/_card.json",
        test::card_json(R"({"type":"trigger"})")},
       {"definitions/draft/_card.json",
        test::card_json(R"({"type":"draft"})")}});
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
      {{"definitions/src/_card.json", test::card_json(R"({"type":"code"})")},
       {"definitions/src/_outputs.fbs", kSchema},
       {"definitions/dest/_card.json",
        test::card_json(R"({"type":"code","inputs":[{"name":"data"}]})")},
       {"definitions/container/_card.json",
        test::card_json(R"({"type":"subgraph"})")},
       {"externals/container/tsugumi.json",
        test::project_json(
            R"({"canvas":{"cards":[{"id":"inner"}],"connections":[]},"boundary":{"inputs":[{"name":"in","card":"inner","port":"in"}],"outputs":[{"name":"out","card":"inner","port":"out"}]}})")},
       {"externals/container/definitions/inner/_card.json",
        test::card_json(R"({"type":"subgraph"})")},
       {"externals/container/externals/inner/tsugumi.json",
        test::project_json(
            R"({"canvas":{"cards":[{"id":"leaf"}],"connections":[]},"boundary":{"inputs":[{"name":"in","card":"leaf","port":"in"}],"outputs":[{"name":"out","card":"leaf","port":"out"}]}})")},
       {"externals/container/externals/inner/definitions/leaf/_card.json",
        test::card_json(R"({"type":"code","inputs":[{"name":"in"}]})")},
       {"externals/container/externals/inner/definitions/leaf/_outputs.fbs",
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
TEST(InputTypes, CollectAndMergeUseTheResolverTypes) {
  const auto graph = Graph(
      R"([{"id":"src"},{"id":"collect"},{"id":"merge"}])",
      R"([{"from":{"card":"src","port":"out"},"to":{"card":"collect","port":"in"}},
          {"from":{"card":"collect","port":"out"},"to":{"card":"merge","port":"items"}}])",
      {{"definitions/src/_card.json", test::card_json(R"({"type":"code"})")},
       {"definitions/src/_outputs.fbs", kSchema},
       {"definitions/collect/_card.json",
        test::card_json(R"({"type":"collect"})")},
       {"definitions/merge/_card.json",
        test::card_json(R"({"type":"merge","inputs":[{"name":"items"}]})")}});
  for (const auto& [card, port] :
       std::vector<std::pair<std::string, std::string>>{
           {"collect", "out"},
           {"merge", "out"}}) {
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
      {{"definitions/first/_card.json", test::card_json(R"({"type":"code"})")},
       {"definitions/first/_outputs.fbs",
        "table First { value:int; } table Outputs { out:First; } root_type "
        "Outputs;"},
       {"definitions/second/_card.json", test::card_json(R"({"type":"code"})")},
       {"definitions/second/_outputs.fbs",
        "table Second { value:int; } table Outputs { out:Second; } root_type "
        "Outputs;"},
       {"definitions/dest/_card.json",
        test::card_json(R"({"type":"code","inputs":[{"name":"in"}]})")}});
  const auto ends = input_types(*graph, {"dest"});
  ASSERT_EQ(ends.size(), 1u);
  EXPECT_EQ(ends[0].source->path, (CardPath{"first"}));
  EXPECT_EQ(ends[0].type, graph->find({"dest"})->input("in"));
  EXPECT_EQ(ends[0].type->type, graph->find({"first"})->output("out")->type);
}
TEST(InputTypes, SchemaIdCanonicalisesReachableTypes) {
  const auto parse = [](const std::string& text, const std::string& file) {
    const auto result = types::parse_catalog(text, file);
    EXPECT_TRUE(result.errors.empty())
        << (result.errors.empty() ? "" : result.errors.front());
    if (result.ports.empty() || !result.ports.front().type) return std::string{};
    return schema_id(*result.ports.front().type);
  };
  const std::string first =
      "namespace example; table Member { x:int = 2; } "
      "table Message { child:Member; } "
      "table Outputs { direct:Message; } root_type Outputs;";
  const std::string second =
      "namespace example; table AUnrelated { other:long; }\n"
      "/// Documentation is not part of the type.\n"
      "table Message { child:Member; } "
      "table Member { x:int = 2; } "
      "table Catalogue { another:Message; } root_type Catalogue;";
  const auto id = parse(first, "definitions/a/_outputs.fbs");
  ASSERT_EQ(id.size(), 64u);
  EXPECT_EQ(id, parse(second, "definitions/b/_outputs.fbs"));
  auto changed = second;
  changed.replace(changed.find("x:int = 2"), 9, "x:int = 3");
  EXPECT_NE(id, parse(changed, "definitions/b/_outputs.fbs"));
}

TEST(InputTypes, GeneratedSchemaKeepsMergeCandidatesAndCollectMembers) {
  for (const bool same : {false, true}) {
    SCOPED_TRACE(same);
    const auto graph = Graph(
        R"([{"id":"a"},{"id":"b"},{"id":"merge"},{"id":"collect"},{"id":"dest"}])",
        R"([{"from":{"card":"a","port":"out"},"to":{"card":"merge","port":"a"}},
            {"from":{"card":"b","port":"out"},"to":{"card":"merge","port":"b"}},
            {"from":{"card":"merge","port":"out"},"to":{"card":"collect","port":"in"}},
            {"from":{"card":"merge","port":"out"},"to":{"card":"dest","port":"merged"}},
            {"from":{"card":"collect","port":"out"},"to":{"card":"dest","port":"batch"}}])",
        {{"definitions/a/_card.json", test::card_json(R"({"type":"code"})")},
         {"definitions/b/_card.json", test::card_json(R"({"type":"code"})")},
         {"definitions/a/_outputs.fbs", kSchema},
         {"definitions/b/_outputs.fbs", same ? kSchema : R"(
            include "tsugumi/artifact.fbs";
            namespace sample;
            table Message { artifact:tsugumi.Artifact; }
            table Outputs { out:Message; } root_type Outputs;)"},
         {"definitions/merge/_card.json",
          test::card_json(
              R"({"type":"merge","inputs":[{"name":"a"},{"name":"b"}]})")},
         {"definitions/collect/_card.json",
          test::card_json(R"({"type":"collect"})")},
         {"definitions/dest/_card.json",
          test::card_json(
              R"({"type":"code","inputs":[{"name":"unwired"},{"name":"merged"},{"name":"batch"}]})")}});
    const auto* node = graph->find({"dest"});
    ASSERT_TRUE(node->generated_inputs);
    const auto& schema = *node->generated_inputs;
    EXPECT_TRUE(schema.starts_with("// Generated by Tsugumi"));
    EXPECT_FALSE(schema.contains("unwired:"));
    const auto id = schema_id(*graph->find({"a"})->output("out")->type);
    const auto candidate_ns = same ? "" : "s" + id.substr(0, 12) + ".";
    EXPECT_TRUE(schema.contains("namespace inputs.batch." + candidate_ns +
                                "sample;"));
    EXPECT_TRUE(schema.contains("batch: tsugumi.builtin.Collected"));
    const auto collected_id = schema_id(*graph->find({"collect"})->output("out")->type);
    EXPECT_TRUE(schema.contains("batch: tsugumi.builtin.Collected (id: " +
                                std::to_string(same ? 1 : 2) +
                                ", tsugumi_schema: \"" + collected_id + "\");"));
    EXPECT_EQ(schema.contains("union merged_candidates"), !same);
    EXPECT_FALSE(schema.contains("namespace inputs.merged.tsugumi;"));
    EXPECT_TRUE(schema.contains("tsugumi_schema: \"" + id + "\""));
    std::string error, root;
    ASSERT_TRUE(types::schema_bfbs(schema, &root, &error)) << error << '\n'
                                                           << schema;
    EXPECT_EQ(root, "inputs.Inputs");
    const auto collected =
        types::compile(schema, "tsugumi.builtin.Collected", &error);
    ASSERT_TRUE(collected) << error;
    EXPECT_TRUE(schema.contains("batch: tsugumi.builtin.Collected (id: " +
                                std::to_string(same ? 1 : 2) +
                                ", tsugumi_schema: \"" +
                                schema_id(**collected) + "\");"));
    EXPECT_FALSE(schema.contains("table Collected (tsugumi_schema:"));
  }
}

TEST(InputTypes, DirectArtifactRootStampsTheSharedTypeOnTheField) {
  const auto graph = Graph(
      R"([{"id":"source"},{"id":"dest"}])",
      R"([{"from":{"card":"source","port":"out"},
           "to":{"card":"dest","port":"in"}}])",
            {{"definitions/source/_card.json",
              test::card_json(R"({"type":"input"})")},
             {"definitions/source/_outputs.fbs",
              R"(include "tsugumi/artifact.fbs";
           table Outputs { out:tsugumi.Artifact; } root_type Outputs;)"},
             {"definitions/dest/_card.json",
              test::card_json(R"({"type":"code","inputs":[{"name":"in"}]})")}});
  const auto& schema = graph->find({"dest"})->generated_inputs;
  ASSERT_TRUE(schema);
  const auto id = schema_id(*graph->find({"source"})->output("out")->type);
  EXPECT_TRUE(schema->contains("include \"tsugumi/artifact.fbs\";"));
  EXPECT_TRUE(schema->contains("in: tsugumi.Artifact (id: 0, tsugumi_schema: \"" +
                               id + "\");"));
  EXPECT_FALSE(schema->contains("table Artifact"));
  std::string root, error;
  EXPECT_TRUE(types::schema_bfbs(*schema, &root, &error)) << error;
}

TEST(InputTypes, OutputIncludesReadOnlyConfirmedDefinitionFiles) {
  const auto parsed = types::parse_catalog(
      R"(include "part.fbs";
      include "tsugumi/artifact.fbs";
      table Outputs { out:Part; } root_type Outputs;)",
      "definitions/a/_outputs.fbs",
      [](const std::string& path) -> std::optional<std::string> {
        if (path == "definitions/a/part.fbs")
          return "table Part { mapped:tsugumi.Artifact; }";
        return std::nullopt;
      });
  EXPECT_TRUE(parsed.errors.empty());
  ASSERT_EQ(parsed.ports.size(), 1u);
  ASSERT_TRUE(parsed.ports.front().type);
  EXPECT_EQ(schema_id(*parsed.ports.front().type).size(), 64u);
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
  std::string rendered;
  types::render_closure(*parsed.ports[0].type, "inputs.first", rendered);
  EXPECT_NE(rendered.find("nested_flatbuffer: \"inputs.first.Sample.Inner\""),
            std::string::npos);
}

TEST(InputTypes, LargeDefinitionAndModelResultAreNeverClipped) {
  std::string schema = "table Large {\n";
  for (int i = 0; i < 5000; ++i)
    schema += "  field_" + std::to_string(i) + ":double;\n";
  schema += "}\n";
  const auto graph = Graph(
      R"([{"id":"src"},{"id":"draft"}])",
      R"([{"from":{"card":"src","port":"out"},"to":{"card":"draft"}}])",
      {{"definitions/src/_card.json", test::card_json(R"({"type":"code"})")},
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
