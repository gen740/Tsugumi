#include "host/file_miss.hh"

#include <gtest/gtest.h>

namespace tsugumi::host {
namespace {
namespace json = ::tsugumi::json;
auto Snapshot(const std::string& kind) -> json::Members {
  return {{"rev", "\"7\""},
          {"definitions",
           json::Encode(std::vector<std::string>{json::Encode(json::Members{
               {"id", "\"share\""},
               {"card_json", json::Quote("{\"type\":\"" + kind + "\"}")},
               {"readme", "\"Card description\""}})})}};
}
TEST(FileMiss, BuiltinSchemasDescribeTheCardFilesAndResolvedTypeTool) {
  for (const auto* kind :
       {"merge", "collect", "trigger", "draft"}) {
    for (const auto* file : {"_outputs.fbs", "_inputs.fbs"}) {
      const auto diagnostic = DescribeFileMiss(
          "definitions/share/" + std::string(file), Snapshot(kind), {});
      EXPECT_FALSE(diagnostic.empty());
      EXPECT_NE(diagnostic.find("share"), std::string::npos);
    }
  }
}
TEST(FileMiss, LargeFoldersHaveABoundedListAndTotal) {
  auto snapshot = Snapshot("code");
  std::vector<std::string> sources;
  for (int i = 0; i < 100; ++i)
    sources.push_back(json::Encode(
        json::Members{{"path", json::Quote("definitions/share/__fixture_" +
                                           std::to_string(i) + "__")}}));
  snapshot["sources"] = json::Encode(sources);
  const auto diagnostic =
      DescribeFileMiss("definitions/share/missing", snapshot, {});
  EXPECT_FALSE(diagnostic.empty());
  size_t listed = 0;
  for (int i = 0; i < 100; ++i) {
    const auto file = "__fixture_" + std::to_string(i) + "__";
    if (diagnostic.find(file) != std::string::npos) ++listed;
  }
  EXPECT_GT(listed, 0u);
  EXPECT_LT(listed, sources.size());
  EXPECT_NE(diagnostic.find(std::to_string(sources.size() + 2)),
            std::string::npos) << diagnostic;
  EXPECT_EQ(DescribeFileMiss("definitions/share/__fixture_0__", snapshot, {}),
            "");
  EXPECT_EQ(DescribeFileMiss("definitions/absent/missing", snapshot, {}), "");
  EXPECT_EQ(DescribeFileMiss("elsewhere/missing", snapshot, {}), "");
}
TEST(FileMiss, CodeSchemaMissDoesNotClaimCodeHasNoSchemas) {
  EXPECT_FALSE(
      DescribeFileMiss("definitions/share/_outputs.fbs", Snapshot("code"), {})
          .empty());
  EXPECT_FALSE(
      DescribeFileMiss(
          "definitions/share/missing",
          {{"definitions",
            R"([{"id":"share","card_json":"{\"type\":\"code\",\"symbol\":\"chevron.left.forwardslash.chevron.right\",\"inputs\":[],\"maxRuns\":1,\"reuseWorker\":false,\"timeoutSeconds\":0,\"buildTimeoutSeconds\":0,\"recordLogs\":\"project\",\"recordIO\":\"project\",\"sandbox\":{\"filesystem\":true,\"pid\":true,\"ipc\":true,\"identity\":true,\"limits\":{\"cpu\":0,\"memoryMiB\":0}},\"grants\":{\"network\":false,\"gui\":false},\"system\":\"aarch64-linux\",\"workerCapabilities\":[]}"}])"}},
          {})
          .empty());
  // Without a type the Card is not described as any kind.
  EXPECT_EQ(DescribeFileMiss(
                "definitions/share/missing",
                {{"definitions", R"([{"id":"share","card_json":"{}"}])"}}, {}),
            "");
}
TEST(FileMiss, ExternalCardMetadataIsReadAtTheInspectionRevision) {
  const std::string root = "externals/one/externals/two/definitions/share/";
  auto snapshot = json::Members{
      {"rev", "\"7\""},
      {"sources", json::Encode(std::vector<std::string>{
                      json::Encode(json::Members{
                          {"path", json::Quote(root + "_card.json")}}),
                      json::Encode(json::Members{
                          {"path", json::Quote(root + "README.md")}})})}};
  int reads = 0;
  const auto diagnostic = DescribeFileMiss(
      root + "_outputs.fbs", snapshot,
      [&](const auto& method, const auto& args) -> json::Members {
        ++reads;
        EXPECT_EQ(method, "project_file_read");
        EXPECT_EQ(json::Text(args, "path"), root + "_card.json");
        EXPECT_EQ(json::Text(args, "at_rev"), "7");
        return {
            {"bytes",
             json::Quote(
                 R"({"type":"collect","symbol":"line.3.horizontal.decrease","maxRuns":1,"recordIO":"project"})")}};
      });
  EXPECT_EQ(reads, 1);
  EXPECT_FALSE(diagnostic.empty());
  EXPECT_TRUE(DescribeFileMiss(
                  root + "_outputs.fbs", snapshot,
                  [](const auto&, const auto&) -> json::Members { return {}; })
                  .empty());
}
}  // namespace
}  // namespace tsugumi::host
