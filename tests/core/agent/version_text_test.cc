#include "host/version_text.hh"

#include <gtest/gtest.h>

#include "util/crypto.hh"
#include "util/fs.hh"
#include "version_record.hh"
#include "versions.hh"

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
  const auto first = versions.init();
  EXPECT_EQ(host::ChangeText(versions.note(first).record_json, true), "Added a; Added b");
  Put("definitions/a/_card.json",
      R"({"language":"python","params":[{"name":"secret","data":"PRIVATE_SENTINEL"}]})");
  const auto params = versions.commit_all("Parameters");
  const auto neutral = versions.note(params).record_json;
  EXPECT_EQ(host::ChangeText(neutral, true), "Changed a (parameters)");
  EXPECT_EQ(versions.note(params).record_json, neutral);
  EXPECT_EQ(neutral.find("PRIVATE_SENTINEL"), std::string::npos);
  EXPECT_EQ(host::ChangeText(versions.note(params).record_json, false), "変更: a (パラメータ)");
  EXPECT_TRUE(host::ReadSummary(root, params, true).empty());
  Put("definitions/a/_card.json",
      "{ \"params\": [ { \"data\": \"PRIVATE_SENTINEL\", \"name\": \"secret\" } ], \"language\": \"python\" "
      "}");
  const auto formatting = versions.commit_all("Formatting");
  EXPECT_EQ(host::ChangeText(versions.note(formatting).record_json, true),
            "No Card definition, source, parameter or wiring changes");
  Put("definitions/a/_card.json",
      R"({"language":"python","network":true,"params":[{"name":"secret","data":"PRIVATE_SENTINEL"}]})");
  Put("definitions/a/src/main.py", "# CHANGED_SOURCE_SENTINEL\n" + std::string(15000, '#'));
  Put("definitions/c/_card.json", "{}");
  std::filesystem::remove_all(root / "definitions/b");
  Put("tsugumi.json",
      R"({"canvas":{"cards":[{"id":"a"},{"id":"c"}],"connections":[{"from":{"card":"a","port":"out"},"to":{"card":"c","port":"in"}}]}})");
  auto change = versions.commit_all("Change");
  EXPECT_TRUE(host::VersionDiff(root, change).truncated);
  EXPECT_EQ(host::ChangeText(versions.note(change).record_json, true),
            "Added c; Removed b; Changed a (definition, source, wiring)");
  host::StoreSummary(root, change, true, "An Agent note");
  EXPECT_EQ(host::ReadSummary(root, change, true), "An Agent note");
  EXPECT_TRUE(host::ReadSummary(root, change, false).empty());
  EXPECT_EQ(host::ChangeText(versions.note(change).record_json, true).find("SENTINEL"), std::string::npos);
  Put("layout.json", R"({"cards":{"a":{"x":42,"y":1}}})");
  Put("definitions/a/data/data.csv", "PRIVATE_DATA_SENTINEL");
  change = versions.commit_all("Layout and data");
  EXPECT_EQ(host::ChangeText(versions.note(change).record_json, true),
            "No Card definition, source, parameter or wiring changes");
  EXPECT_TRUE(host::VersionDiff(root, change).text.empty());
  EXPECT_THROW(versions.note(std::string(40, '0')), std::exception);
  Put("definitions/a/src/main.rs", "fn main() {}");
  Put("definitions/a/src/_card.json", "not a Card document");
  change = versions.commit_all("Source inputs");
  EXPECT_EQ(host::ChangeText(versions.note(change).record_json, true), "Changed a (source)");
  EXPECT_TRUE(host::VersionDiff(root, change).text.empty());
}
TEST_F(VersionNote, NestedCardsAndTheEntryLimitUseTheFullDiff) {
  Put("tsugumi.json", R"({"canvas":{"cards":[{"id":"inner"}],"connections":[]}})");
  Put("externals/inner/tsugumi.json", R"({"canvas":{"cards":[{"id":"a"}],"connections":[]}})");
  Put("externals/inner/definitions/a/_card.json", "{}");
  Versions versions(root);
  versions.init();
  Put("externals/inner/definitions/a/src/main.py", "print('private')");
  auto commit = versions.commit_all("Nested source");
  EXPECT_EQ(host::ChangeText(versions.note(commit).record_json, true), "Changed inner/a (source)");
  Put("externals/inner/tsugumi.json",
      R"({"canvas":{"cards":[{"id":"a"},{"id":"b"},{"id":"c"},{"id":"d"},{"id":"e"},{"id":"f"},{"id":"g"},{"id":"h"}],"connections":[]}})");
  commit = versions.commit_all("Many Cards");
  EXPECT_EQ(
      host::ChangeText(versions.note(commit).record_json, true),
      "Added inner/b; Added inner/c; Added inner/d; Added inner/e; Added inner/f; Added inner/g; and 1 more");
  EXPECT_NE(host::ChangeText(versions.note(commit).record_json, false).find("ほか 1 件"), std::string::npos);
}
TEST(VersionText, OneRunRecordCanBeShownInEitherLanguageWithoutTruncatingFacts) {
  const std::string record = R"({"whole":false,"card":"split","propagate":true,"saved_inputs":true,
    "ran":[{"card":"split","count":1},{"card":"both","count":2}],
    "failures":[{"card":"both","kind":"execution_limit"}],"never":["sink"]})";
  EXPECT_EQ(
      host::RunText(record, true),
      "from split (with saved inputs): split, both ×2 / failed: both (execution_limit) / not run: sink");
  EXPECT_EQ(host::RunText(record, false),
            "split から（保存済みの入力で）: split、both ×2 / 失敗: both（execution_limit） / 未実行: sink");
  version_record::Run many;
  *many.whole = true;
  for (int i = 0; i < 10; ++i) {
    version_record::Count count;
    *count.card = "c" + std::to_string(i);
    *count.count = 1;
    many.ran->push_back(count);
  }
  EXPECT_TRUE(host::RunText(schema::encode(many), true).ends_with(" and 4 more"));
  EXPECT_TRUE(host::RunText(schema::encode(many), false).ends_with(" ほか 4"));
  EXPECT_EQ(many.ran->size(), 10u);
}
TEST_F(VersionNote, NeutralCommitsAndRunNotesSurviveReopening) {
  Put("tsugumi.json", "{}");
  Versions versions(root);
  const auto commit = versions.init();
  const auto message = versions.graph(1).commits.front().message;
  EXPECT_EQ(host::CommitText(message, true), "Create the project");
  EXPECT_EQ(host::CommitText(message, false), "プロジェクトを作成");
  EXPECT_EQ(host::CommitText("User-authored words", false), "User-authored words");
  EXPECT_EQ(host::CommitText(version_record::commit("checkout"), true), "Changes in progress");
  EXPECT_EQ(host::CommitText(version_record::commit("run", "inner/a"), false), "実行: inner/a");
  version_record::Run record;
  *record.whole = true;
  VersionRunNote run{"run-id", "succeeded", "2026-11-15T00:00:00Z", 1.25, schema::encode(record)};
  EXPECT_EQ(Versions::format_note(run).find("failed"), std::string::npos);
  versions.note_run(commit, run);
  Versions reopened(root);
  const auto facts = reopened.graph(1);
  ASSERT_EQ(facts.commits.front().runs.size(), 1u);
  EXPECT_EQ(facts.commits.front().runs.front().seconds, 1.25);
  EXPECT_EQ(host::RunText(facts.commits.front().runs.front().record_json, false), "全体");
  EXPECT_EQ(host::RunText(facts.commits.front().runs.front().record_json, true), "whole");
}
TEST_F(VersionNote, HostDiffComparesTheChosenVersionsWithoutChangingGit) {
  Put("tsugumi.json", "{}");
  Put("definitions/a/src/main.py", "# first\n");
  Put("definitions/a/src/values.csv", "PRIVATE_DATA\n");
  Versions versions(root);
  const auto first = versions.init();
  Put("definitions/a/src/main.py", "# intermediate\n");
  versions.commit_all("Intermediate");
  Put("definitions/a/src/main.py", "# third\n");
  const auto third = versions.commit_all("Third");
  const auto diff = host::VersionDiff(root, third, first);
  EXPECT_NE(diff.text.find("first"), std::string::npos);
  EXPECT_NE(diff.text.find("third"), std::string::npos);
  EXPECT_EQ(diff.text.find("intermediate"), std::string::npos);
  EXPECT_EQ(host::VersionDiff(root, first).text.find("PRIVATE_DATA"), std::string::npos);
  EXPECT_TRUE(host::VersionDiff(root, third, third).text.empty());
  EXPECT_EQ(versions.graph(1).head, third);
  EXPECT_FALSE(versions.graph(1).dirty);
  EXPECT_THROW(host::VersionDiff(root, third, std::string(40, '0')), std::exception);
  Tsugumi::RPC::VersionGraphT graph;
  auto commit = std::make_unique<Tsugumi::RPC::VersionCommitT>();
  commit->message = version_record::commit("run", "a");
  commit->summary = commit->message;
  auto run = std::make_unique<Tsugumi::RPC::VersionRunT>();
  version_record::Run record;
  *record.whole = true;
  run->summary = schema::encode(record);
  commit->runs.push_back(std::move(run));
  graph.commits.push_back(std::move(commit));
  auto japanese = graph;
  host::PresentGraph(graph, true);
  host::PresentGraph(japanese, false);
  EXPECT_EQ(graph.commits.front()->message, "Run: a");
  EXPECT_EQ(japanese.commits.front()->message, "実行: a");
  EXPECT_EQ(graph.commits.front()->runs.front()->summary, "whole");
  EXPECT_EQ(japanese.commits.front()->runs.front()->summary, "全体");
}
TEST(VersionText, ModelReadPolicyLivesOnTheHost) {
  EXPECT_TRUE(host::SummaryReads("externals/p/definitions/a/src/main.py"));
  EXPECT_TRUE(host::SummaryReads("definitions/a/_meta.json"));
  EXPECT_FALSE(host::SummaryReads("definitions/a/src/data.csv"));
  EXPECT_FALSE(host::SummaryReads("definitions/a/data/private.py"));
  EXPECT_FALSE(host::SummaryReads("layout.json"));
}
}  // namespace
}  // namespace tsugumi
