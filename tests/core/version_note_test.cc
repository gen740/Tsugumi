#include "versions.hh"

#include <gtest/gtest.h>

#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi {
namespace {
class VersionNote : public ::testing::Test {
 protected:
  void SetUp() override {
    root = std::filesystem::temp_directory_path() / ("tsugumi-version-note-" + crypto::random_token(8));
    std::filesystem::create_directory(root);
  }
  void TearDown() override { std::filesystem::remove_all(root); }
  auto Put(const std::string& path, const std::string& text) -> void {
    std::filesystem::create_directories((root / path).parent_path());
    fs::write_atomic(root / path, text, 0644);
  }
  std::filesystem::path root;
};
TEST_F(VersionNote, FullTreesDistinguishParametersDefinitionsSourceWiringAndMembership) {
  Put("tsugumi.json", R"({"canvas":{"cards":[{"id":"a"},{"id":"b"}],"connections":[]}})");
  Put("definitions/a/_card.json", R"({"language":"python","params":[{"name":"secret","data":"FIRST"}]})");
  Put("definitions/b/_card.json", "{}");
  Put("definitions/a/src/main.py", std::string(15000, '#') + "\n");
  Versions versions(root);
  const auto first = versions.init(true);
  EXPECT_EQ(versions.note(first, true).text, "Added a; Added b");
  Put("definitions/a/_card.json", R"({"language":"python","params":[{"name":"secret","data":"PRIVATE_SENTINEL"}]})");
  const auto params = versions.commit_all("Parameters");
  EXPECT_EQ(versions.note(params, true).text, "Changed a (parameters)");
  EXPECT_EQ(versions.note(params, false).text, "変更: a (パラメータ)");
  EXPECT_TRUE(versions.note(params, true).agent_summary.empty());
  Put("definitions/a/_card.json", "{ \"params\": [ { \"data\": \"PRIVATE_SENTINEL\", \"name\": \"secret\" } ], \"language\": \"python\" }");
  const auto formatting = versions.commit_all("Formatting");
  EXPECT_EQ(versions.note(formatting, true).text, "No Card definition, source, parameter or wiring changes");
  Put("definitions/a/_card.json", R"({"language":"python","network":true,"params":[{"name":"secret","data":"PRIVATE_SENTINEL"}]})");
  Put("definitions/a/src/main.py", "# CHANGED_SOURCE_SENTINEL\n" + std::string(15000, '#'));
  Put("definitions/c/_card.json", "{}");
  std::filesystem::remove_all(root / "definitions/b");
  Put("tsugumi.json", R"({"canvas":{"cards":[{"id":"a"},{"id":"c"}],"connections":[{"from":{"card":"a","port":"out"},"to":{"card":"c","port":"in"}}]}})");
  auto change = versions.commit_all("Change");
  EXPECT_TRUE(versions.diff(change).truncated);
  EXPECT_EQ(versions.note(change, true).text, "Added c; Removed b; Changed a (definition, source, wiring)");
  versions.set_summary(change, "An Agent note");
  EXPECT_EQ(versions.note(change, true).agent_summary, "An Agent note");
  EXPECT_EQ(versions.note(change, false).agent_summary, "An Agent note");
  EXPECT_EQ(versions.note(change, true).text.find("SENTINEL"), std::string::npos);
  Put("layout.json", R"({"cards":{"a":{"x":42,"y":1}}})");
  Put("definitions/a/data/data.csv", "PRIVATE_DATA_SENTINEL");
  change = versions.commit_all("Layout and data");
  EXPECT_EQ(versions.note(change, true).text, "No Card definition, source, parameter or wiring changes");
  EXPECT_TRUE(versions.diff(change).text.empty());
  EXPECT_THROW(versions.note(std::string(40, '0'), true), std::exception);
  Put("definitions/a/src/main.rs", "fn main() {}");
  Put("definitions/a/src/_card.json", "not a Card document");
  change = versions.commit_all("Source inputs");
  EXPECT_EQ(versions.note(change, true).text, "Changed a (source)");
  EXPECT_TRUE(versions.diff(change).text.empty());
}
TEST_F(VersionNote, NestedCardsAndTheEntryLimitUseTheFullDiff) {
  Put("tsugumi.json", R"({"canvas":{"cards":[{"id":"inner"}],"connections":[]}})");
  Put("externals/inner/tsugumi.json", R"({"canvas":{"cards":[{"id":"a"}],"connections":[]}})");
  Put("externals/inner/definitions/a/_card.json", "{}");
  Versions versions(root);
  versions.init(true);
  Put("externals/inner/definitions/a/src/main.py", "print('private')");
  auto commit = versions.commit_all("Nested source");
  EXPECT_EQ(versions.note(commit, true).text, "Changed inner/a (source)");
  Put("externals/inner/tsugumi.json", R"({"canvas":{"cards":[{"id":"a"},{"id":"b"},{"id":"c"},{"id":"d"},{"id":"e"},{"id":"f"},{"id":"g"},{"id":"h"}],"connections":[]}})");
  commit = versions.commit_all("Many Cards");
  EXPECT_EQ(versions.note(commit, true).text, "Added inner/b; Added inner/c; Added inner/d; Added inner/e; Added inner/f; Added inner/g; and 1 more");
  EXPECT_NE(versions.note(commit, false).text.find("ほか 1 件"), std::string::npos);
}
}  // namespace
}  // namespace tsugumi
