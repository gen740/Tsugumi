#include "host/file_miss.hh"

#include <gtest/gtest.h>

namespace tsugumi::host {
namespace {
namespace json = agent::json;
auto Snapshot(const std::string& kind) -> json::Members {
  return {{"rev", "\"7\""},
          {"definitions",
           json::Encode(std::vector<std::string>{json::Encode(json::Members{
               {"id", "\"share\""},
               {"card_json", json::Quote("{\"template\":\"" + kind + "\"}")},
               {"meta_json", "\"{}\""}})})}};
}
TEST(FileMiss, BuiltinSchemasDescribeTheCardFilesAndResolvedTypeTool) {
  for (const auto* kind :
       {"fork", "branch", "merge", "collect", "trigger", "switch", "draft"}) {
    for (const auto* file : {"_outputs.fbs", "_inputs.fbs"}) {
      const auto diagnostic = DescribeFileMiss(
          "definitions/share/" + std::string(file), Snapshot(kind), {});
      for (const auto& part :
           {std::string(kind), std::string("share"), std::string("_card.json"),
            std::string("_meta.json"), std::string("project_inspect"),
            std::string("root_type"), std::string("schema_bfbs")})
        EXPECT_NE(diagnostic.find(part), std::string::npos) << diagnostic;
      EXPECT_EQ(diagnostic.find('\n'), std::string::npos);
      EXPECT_EQ(diagnostic.find("output_types"), std::string::npos);
    }
  }
}
TEST(FileMiss, LargeFoldersHaveABoundedListAndTotal) {
  auto snapshot = Snapshot("code");
  std::vector<std::string> sources;
  for (int i = 0; i < 100; ++i)
    sources.push_back(json::Encode(json::Members{
        {"path", json::Quote("definitions/share/file" + std::to_string(i))}}));
  snapshot["sources"] = json::Encode(sources);
  const auto diagnostic =
      DescribeFileMiss("definitions/share/missing", snapshot, {});
  EXPECT_NE(diagnostic.find("102"), std::string::npos);
  EXPECT_NE(diagnostic.find("file0"), std::string::npos);
  EXPECT_EQ(diagnostic.find("file99"), std::string::npos);
  EXPECT_LT(diagnostic.size(), 200u);
  EXPECT_EQ(DescribeFileMiss("definitions/share/file0", snapshot, {}), "");
  EXPECT_EQ(DescribeFileMiss("definitions/absent/missing", snapshot, {}), "");
  EXPECT_EQ(DescribeFileMiss("elsewhere/missing", snapshot, {}), "");
}
TEST(FileMiss, CodeSchemaMissDoesNotClaimCodeHasNoSchemas) {
  for (const auto* kind : {"code", "none", ""}) {
    const auto diagnostic =
        DescribeFileMiss("definitions/share/_outputs.fbs", Snapshot(kind), {});
    EXPECT_NE(diagnostic.find("code"), std::string::npos);
    EXPECT_EQ(diagnostic.find("project_inspect"), std::string::npos);
  }
  auto snapshot = Snapshot("code");
  snapshot["definitions"] = R"([{"id":"share","card_json":"{}"}])";
  EXPECT_NE(
      DescribeFileMiss("definitions/share/missing", snapshot, {}).find("code"),
      std::string::npos);
}
TEST(FileMiss, ExternalCardMetadataIsReadAtTheInspectionRevision) {
  const std::string root = "externals/one/externals/two/definitions/share/";
  auto snapshot = json::Members{
      {"rev", "\"7\""},
      {"sources", json::Encode(std::vector<std::string>{
                      json::Encode(json::Members{
                          {"path", json::Quote(root + "_card.json")}}),
                      json::Encode(json::Members{
                          {"path", json::Quote(root + "_meta.json")}})})}};
  int reads = 0;
  const auto diagnostic = DescribeFileMiss(
      root + "_outputs.fbs", snapshot,
      [&](const auto& method, const auto& args) -> json::Members {
        ++reads;
        EXPECT_EQ(method, "project_file_read");
        EXPECT_EQ(json::Text(args, "path"), root + "_card.json");
        EXPECT_EQ(json::Text(args, "at_rev"), "7");
        return {{"bytes", json::Quote(R"({"template":"fork"})")}};
      });
  EXPECT_EQ(reads, 1);
  EXPECT_NE(diagnostic.find("fork"), std::string::npos);
  EXPECT_NE(diagnostic.find("project_inspect"), std::string::npos);
}
}  // namespace
}  // namespace tsugumi::host
