#include "agent/tool_views.hh"

#include <flatbuffers/idl.h>
#include <gtest/gtest.h>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::agent {
namespace {
namespace json = ::tsugumi::json;

constexpr const char* kSchema =
    "namespace report; table Summary { region:string; total:ulong; share:double; }"
    " root_type Summary;";

auto Bytes(const flatbuffers::FlatBufferBuilder& b) -> std::string {
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}

auto Base64(const std::string& bytes) -> std::string {
  return json::Encode(json::Members{{"base64", json::Quote(crypto::base64(bytes))}});
}

// A NamedValue as the project RPC's JSON form carries it.
auto RecordedValue(const std::string& data_json) -> std::string {
  flatbuffers::Parser schema;
  EXPECT_TRUE(schema.Parse(kSchema)) << schema.error_;
  schema.Serialize();
  const auto bfbs = Bytes(schema.builder_);
  flatbuffers::Parser data;
  EXPECT_TRUE(data.Parse(kSchema) && data.Parse(data_json.c_str())) << data.error_;
  return json::Encode(json::Members{{"name", json::Quote("summary")},
                                    {"root_type", json::Quote("report.Summary")},
                                    {"schema_bfbs", Base64(bfbs)},
                                    {"inline_bytes", Base64(Bytes(data.builder_))},
                                    {"size_bytes", json::Quote("40")}});
}

TEST(ToolViews, RecordedValuesReadAsJsonWithoutTheirBinarySchema) {
  const auto record = json::Encode(json::Members{
      {"reply", json::Quote("RecordResult")},
      {"value", json::Encode(json::Members{
                    {"record", json::Encode(json::Members{
                                   {"outputs", "[" + RecordedValue(R"({"region":"West","total":98322,"share":0.2906})") + "]"},
                                   {"inputs", "[]"}})}})}});
  const auto text = ModelView("run_record", {}, record);
  EXPECT_EQ(text.find("schema_bfbs"), std::string::npos) << text;
  EXPECT_EQ(text.find("inline_bytes"), std::string::npos) << text;
  const auto value = json::Object(json::Get(
      json::Object(json::Array(json::Get(
          json::Object(json::Get(json::Object(json::Get(json::Object(text), "value")), "record")),
          "outputs"))[0]),
      "value"));
  EXPECT_EQ(json::Text(value, "region"), "West");
  EXPECT_EQ(json::Get(value, "total"), "98322");
}

TEST(ToolViews, AValueThatDoesNotMatchItsSchemaSaysSo) {
  auto value = json::Object(RecordedValue(R"({"region":"West"})"));
  value["inline_bytes"] = Base64("not a flatbuffer");
  const auto text = DecodeRecordedValues(json::Encode(value));
  EXPECT_NE(text.find("decode_error"), std::string::npos) << text;
  EXPECT_EQ(text.find("schema_bfbs"), std::string::npos) << text;
}

TEST(ToolViews, InspectSummaryListsCardsWiresFilesAndTheLastRun) {
  const auto snapshot = json::Object(R"({
    "project_id":"p","rev":"3","execution_rev":"2",
    "project_json":"{\"name\":\"Report\",\"recordIO\":false,\"canvas\":{\"cards\":[{\"id\":\"start\"},{\"id\":\"sum\"}],\"connections\":[{\"from\":{\"card\":\"start\",\"port\":\"out\"},\"to\":{\"card\":\"sum\",\"port\":\"go\"}}]}}",
    "layout":"{\"notes\":[{\"id\":\"n1\",\"text\":\"Try a seed\"}]}",
    "definitions":[{"id":"sum","card_json":"{\"type\":\"code\"}","readme":"---\ntitle: Sum\ndescription: Adds rows\n---\nBody"}],
    "sources":[{"path":"definitions/sum/params.nix","hash":"h","exists":true}],
    "resolution":[{"card":{"segments":["sum"]},"inputs":[{"name":"go","root_type":"tsugumi.builtin.Empty","schema_bfbs":{"base64":"AA=="}}],
                   "outputs":[{"name":"total","root_type":"report.Summary"}],"parameters":[{"name":"seed","type":"int","value_json":"42"}],"diagnostics":[]}],
    "diagnostics":[],"run":{"run_id":"1","state":"failed","failure":{"diagnostic":"KeyError"},"started_at":"9"}})");
  const auto summary = InspectSummary(snapshot);
  EXPECT_EQ(json::Get(summary, "wires"), R"(["start.out -> sum.go"])");
  EXPECT_EQ(json::Get(summary, "files"), R"(["definitions/sum/params.nix"])");
  EXPECT_EQ(json::Get(summary, "notes"), R"(["Try a seed"])");
  EXPECT_EQ(json::Text(json::Object(json::Get(summary, "settings")), "name"), "Report");
  const auto card = json::Object(json::Array(json::Get(summary, "cards"))[0]);
  EXPECT_EQ(json::Text(card, "title"), "Sum");
  EXPECT_EQ(json::Text(card, "description"), "Adds rows");
  EXPECT_EQ(json::Text(card, "type"), "code");
  EXPECT_EQ(json::Get(card, "outputs"), R"(["total: report.Summary"])");
  EXPECT_EQ(json::Get(json::Object(json::Get(card, "params")), "seed"), "42");
  const auto run = json::Object(json::Get(summary, "run"));
  EXPECT_EQ(json::Text(run, "state"), "failed");
  EXPECT_FALSE(run.contains("started_at"));
  EXPECT_EQ(json::Encode(summary).find("schema_bfbs"), std::string::npos);
}

TEST(ToolViews, DetailKeepsTheSnapshot) {
  const auto reply = R"({"reply":"ProjectSnapshot","value":{"rev":"1","project_json":"{}","resolution":[]}})";
  const auto full = json::Object(ModelView("project_inspect", {{"detail", "true"}}, reply));
  EXPECT_TRUE(json::Object(json::Get(full, "value")).contains("resolution"));
  const auto summary = json::Object(ModelView("project_inspect", {}, reply));
  EXPECT_FALSE(json::Object(json::Get(summary, "value")).contains("resolution"));
}

TEST(ToolViews, ASaveReportsWhatChangedAndItsDiagnostics) {
  const auto reply = R"({"reply":"SaveResult","value":{"rev":"2","execution_rev":"1","replayed":false,"comments":[],
    "delta":{"origin":"agent","project_json":"{}","layout_changed":false,
      "definitions_upsert":[{"id":"top","card_json":"{}","readme":"long"}],"definitions_removed":[],
      "sources_upsert":[{"path":"definitions/top/src/main.py","hash":"h"}],"sources_removed":[],
      "resolution":[{"card":{"segments":["top"]},"inputs":[{"name":"summary","root_type":"report.Summary"}],"outputs":[],"parameters":[],
                     "diagnostics":[{"severity":"error","message":"bad port"}]},
                    {"card":{"segments":["other"]},"inputs":[],"outputs":[],"parameters":[],"diagnostics":[]}],
      "diagnostics":[]}}})";
  const auto value = json::Object(json::Get(json::Object(ModelView("project_edit", {}, reply)), "value"));
  EXPECT_FALSE(value.contains("delta"));
  EXPECT_FALSE(value.contains("replayed"));
  const auto changed = json::Object(json::Get(value, "changed"));
  EXPECT_EQ(json::Get(changed, "definitions"), R"(["top"])");
  EXPECT_EQ(json::Get(changed, "sources"), R"(["definitions/top/src/main.py"])");
  EXPECT_EQ(json::Get(changed, "tsugumi.json"), "true");
  const auto diagnostics = json::Array(json::Get(value, "diagnostics"));
  ASSERT_EQ(diagnostics.size(), 1u);
  EXPECT_EQ(json::Text(json::Object(diagnostics[0]), "card"), "top");
  const auto cards = json::Array(json::Get(value, "cards"));
  ASSERT_EQ(cards.size(), 1u);
  EXPECT_EQ(json::Get(json::Object(cards[0]), "inputs"), R"(["summary: report.Summary"])");
}

TEST(ToolViews, AStillRunningRunNamesTheCallThatWaitsAgain) {
  const auto reply = R"({"reply":"RunAccepted","still_running":true,"wait_reason":"timeout","value":{"run":{"run_id":"7","state":"running"}}})";
  const auto text = ModelView("run", {{"wait_seconds", "600"}}, reply);
  const auto next = json::Object(json::Get(json::Object(text), "continue"));
  EXPECT_EQ(json::Text(next, "tool"), "run_status");
  const auto arguments = json::Object(json::Get(next, "arguments"));
  EXPECT_EQ(json::Text(arguments, "run_id"), "7");
  EXPECT_EQ(json::Get(arguments, "wait_seconds"), "600");
  const auto ended = R"({"reply":"RunStatus","still_running":false,"value":{"run_id":"7","state":"succeeded"}})";
  EXPECT_FALSE(json::Object(ModelView("run_status", {}, ended)).contains("continue"));
}

}  // namespace
}  // namespace tsugumi::agent
