#include "host/version_text.hh"

#include <gtest/gtest.h>

#include <map>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "version_facts.hh"
#include "version_record.hh"
#include "tsugumi/util/flat_file.hh"
#include "versions.hh"
#include "tsugumi/document/document.hh"

namespace tsugumi {
namespace {
auto ExpectChanges(
    const std::string& raw, const std::vector<std::string>& added,
    const std::vector<std::string>& removed,
    const std::map<std::string, std::vector<std::string>>& changed) -> void {
  const auto record = version_facts::changes(raw);
  EXPECT_EQ(record.added, added);
  EXPECT_EQ(record.removed, removed);
  std::map<std::string, std::vector<std::string>> found;
  for (const auto& item : record.changed)
    found.emplace(item->card, item->kinds);
  EXPECT_EQ(found, changed);
}
class VersionNote : public ::testing::Test {
 protected:
  void SetUp() override {
    root = std::filesystem::temp_directory_path() /
           ("tsugumi-version-note-" + crypto::random_token(8));
    std::filesystem::create_directory(root);
  }
  void TearDown() override { std::filesystem::remove_all(root); }
  auto Put(const std::string& path, const std::string& text) -> void {
    std::filesystem::create_directories((root / path).parent_path());
    fs::write_atomic(root / path, text, 0644);
  }
  std::filesystem::path root;
};
TEST_F(VersionNote,
       FullTreesDistinguishParametersDefinitionsSourceWiringAndMembership) {
  Put("tsugumi.json",
      R"({"canvas":{"cards":[{"id":"a"},{"id":"b"}],"connections":[]}})");
  Put("definitions/a/_card.json",
      R"({"type":"code","params":[{"name":"secret","type":"string","data":{"string":"FIRST"}}]})");
  Put("definitions/b/_card.json", R"({"type":"code"})");
  Put("definitions/a/src/main.py", std::string(15000, '#') + "\n");
  Versions versions(root);
  const auto first = versions.init();
  ExpectChanges(versions.note(first).facts, {"a", "b"}, {}, {});
  Put("definitions/a/_card.json",
      R"({"type":"code","params":[{"name":"secret","type":"string","data":{"string":"PRIVATE_SENTINEL"}}]})");
  const auto params = versions.commit_all("Parameters");
  const auto neutral = versions.note(params).facts;
  ExpectChanges(neutral, {}, {}, {{"a", {"parameters"}}});
  EXPECT_EQ(versions.note(params).facts, neutral);
  EXPECT_EQ(neutral.find("PRIVATE_SENTINEL"), std::string::npos);
  Put("definitions/a/_card.json",
      "{ \"params\": [ { \"data\": { \"string\": \"PRIVATE_SENTINEL\" }, \"name\": \"secret\", \"type\": \"string\" "
      "} ], \"type\": \"code\" "
      "}");
  const auto formatting = versions.commit_all("Formatting");
  ExpectChanges(versions.note(formatting).facts, {}, {}, {});
  Put("definitions/a/_card.json",
      R"({"type":"code","grants":{"network":true},"params":[{"name":"secret","type":"string","data":{"string":"PRIVATE_SENTINEL"}}]})");
  Put("definitions/a/src/main.py",
      "# CHANGED_SOURCE_SENTINEL\n" + std::string(15000, '#'));
  Put("definitions/c/_card.json", R"({"type":"code"})");
  std::filesystem::remove_all(root / "definitions/b");
  Put("tsugumi.json",
      R"({"canvas":{"cards":[{"id":"a"},{"id":"c"}],"connections":[{"from":{"card":"a","port":"out"},"to":{"card":"c","port":"in"}}]}})");
  auto change = versions.commit_all("Change");
  EXPECT_TRUE(versions.diff(change).truncated);
  ExpectChanges(versions.note(change).facts, {"c"}, {"b"},
                {{"a", {"definition", "source", "wiring"}}});
  EXPECT_EQ(host::ChangeText(
                version_facts::changes(versions.note(change).facts), true)
                .find("SENTINEL"),
            std::string::npos);
  Put("layout.bin", document_text::parse(
      document_text::Schema::Layout, "layout.bin",
      R"({"cards":[{"key":"a","x":42,"y":1}]})"));
  Put("definitions/a/data/data.csv", "PRIVATE_DATA_SENTINEL");
  change = versions.commit_all("Layout and data");
  ExpectChanges(versions.note(change).facts, {}, {}, {});
  EXPECT_TRUE(versions.diff(change).text.empty());
  EXPECT_THROW(versions.note(std::string(40, '0')), std::exception);
  Put("definitions/a/src/main.rs", "fn main() {}");
  Put("definitions/a/src/_card.json", "nested source");
  change = versions.commit_all("Source inputs");
  ExpectChanges(versions.note(change).facts, {}, {}, {{"a", {"source"}}});
  const auto diff = versions.diff(change).text;
  EXPECT_NE(diff.find("fn main() {}"), std::string::npos);
  EXPECT_NE(diff.find("nested source"), std::string::npos);
}
TEST_F(VersionNote, NestedCardsAndTheEntryLimitUseTheFullDiff) {
  Put("tsugumi.json",
      R"({"canvas":{"cards":[{"id":"inner"}],"connections":[]}})");
  Put("externals/inner/tsugumi.json",
      R"({"canvas":{"cards":[{"id":"a"}],"connections":[]}})");
  Put("externals/inner/definitions/a/_card.json", R"({"type":"code"})");
  Versions versions(root);
  versions.init();
  Put("externals/inner/definitions/a/src/main.py", "print('private')");
  auto commit = versions.commit_all("Nested source");
  ExpectChanges(versions.note(commit).facts, {}, {},
                {{"inner/a", {"source"}}});
  Put("externals/inner/tsugumi.json",
      R"({"canvas":{"cards":[{"id":"a"},{"id":"b"},{"id":"c"},{"id":"d"},{"id":"e"},{"id":"f"},{"id":"g"},{"id":"h"}],"connections":[]}})");
  commit = versions.commit_all("Many Cards");
  ExpectChanges(versions.note(commit).facts,
                {"inner/b", "inner/c", "inner/d", "inner/e", "inner/f",
                 "inner/g", "inner/h"},
                {}, {});
}
TEST_F(VersionNote, NeutralCommitsAndRunNotesSurviveReopening) {
  Put("tsugumi.json", initial_project_json("test"));
  Versions versions(root);
  const auto commit = versions.init();
  const auto message = versions.graph(1).commits.front().message;
  EXPECT_EQ(version_facts::commit(message)->kind, "create");
  Tsugumi::RPC::VersionRunFactsT record;
  record.whole = true;
  VersionRunNote run{"run-id", "succeeded", "2026-11-15T00:00:00Z", 1.25,
                     flat::encode<Tsugumi::RPC::VersionRunFacts>(record)};
  versions.note_run(commit, run);
  Versions reopened(root);
  const auto facts = reopened.graph(1);
  ASSERT_EQ(facts.commits.front().runs.size(), 1u);
  EXPECT_EQ(facts.commits.front().runs.front().seconds, 1.25);
  EXPECT_TRUE(version_facts::run(facts.commits.front().runs.front().facts)->whole);
  EXPECT_EQ(facts.commits.front().runs.front().state, "succeeded");
}
TEST_F(VersionNote, DiffIncludesTextAcrossTheDefinitionAndExcludesData) {
  Put("tsugumi.json", initial_project_json("test"));
  Put("definitions/a/binary", std::string("old\0binary", 10));
  Put("definitions/a/large", std::string(1024 * 1024 + 1, 'x'));
  Versions versions(root);
  versions.init();
  Put("definitions/a/binary", "changed from binary");
  Put("definitions/a/large", "changed from oversized");
  Put("definitions/a/added.bin", std::string("new\0binary", 10));
  Put("definitions/a/config/settings.toml", "threshold = 3\n");
  Put("definitions/a/README", "definition text\n");
  Put("definitions/a/data/hidden.txt", "data sentinel\n");
  Put("definitions/a/src/measurements.csv", "raw csv sentinel\n");
  Put("otherwise.txt", "outside sentinel\n");
  std::filesystem::create_symlink("README", root / "definitions/a/link");
  const auto diff = versions.diff(versions.commit_all("Change")).text;
  EXPECT_NE(diff.find("threshold = 3"), std::string::npos);
  EXPECT_NE(diff.find("definition text"), std::string::npos);
  EXPECT_EQ(diff.find("definitions/a/binary"), std::string::npos);
  EXPECT_EQ(diff.find("definitions/a/added.bin"), std::string::npos);
  EXPECT_EQ(diff.find("definitions/a/large"), std::string::npos);
  EXPECT_EQ(diff.find("definitions/a/link"), std::string::npos);
  EXPECT_EQ(diff.find("sentinel"), std::string::npos);
}
TEST_F(VersionNote, DiffComparesTheChosenVersionsWithoutChangingGit) {
  Put("tsugumi.json", initial_project_json("test"));
  Put("definitions/a/src/main.py", "# first\n");
  Put("definitions/a/src/values.csv", "PRIVATE_DATA\n");
  Versions versions(root);
  const auto first = versions.init();
  Put("definitions/a/src/main.py", "# intermediate\n");
  versions.commit_all("Intermediate");
  Put("definitions/a/src/main.py", "# third\n");
  const auto third = versions.commit_all("Third");
  const auto diff = versions.diff(third, first);
  EXPECT_NE(diff.text.find("first"), std::string::npos);
  EXPECT_NE(diff.text.find("third"), std::string::npos);
  EXPECT_EQ(diff.text.find("intermediate"), std::string::npos);
  EXPECT_EQ(versions.diff(first).text.find("PRIVATE_DATA"),
            std::string::npos);
  EXPECT_TRUE(versions.diff(third, third).text.empty());
  EXPECT_EQ(versions.graph(1).head, third);
  EXPECT_FALSE(versions.graph(1).dirty);
  EXPECT_THROW(versions.diff(third, std::string(40, '0')),
               std::exception);
}
TEST(VersionText, PublicFactsSeparatePersistenceFromPresentation) {
  auto commit = version_facts::commit(version_record::commit("run", "card"));
  ASSERT_NE(commit, nullptr);
  EXPECT_EQ(commit->kind, "run");
  EXPECT_FALSE(host::CommitText(*commit, true).empty());
  EXPECT_EQ(version_facts::commit("A human message"), nullptr);
  Tsugumi::RPC::VersionRunFactsT persisted;
  persisted.whole = true;
  auto facts = version_facts::run(flat::encode<Tsugumi::RPC::VersionRunFacts>(persisted));
  ASSERT_NE(facts, nullptr);
  EXPECT_TRUE(facts->whole);
  EXPECT_FALSE(host::RunText(*facts, true).empty());
  EXPECT_EQ(version_facts::run(""), nullptr);
  auto changes =
      version_facts::changes(flat::encode<Tsugumi::RPC::VersionChanges>({}));
  EXPECT_TRUE(changes.added.empty());
  EXPECT_FALSE(host::ChangeText(changes, true).empty());
}
TEST_F(VersionNote,
     RecordsFirstParentChangesAndUsesCardIdsAcrossExternalDirectoryAliases) {
  Put("tsugumi.json", initial_project_json("test"));
  Put("definitions/outer-id/_card.json", R"({"type":"subgraph","external":"library"})");
  const auto meta = "externals/library/definitions/inner-id/_comments.json";
  Put(meta, R"({"comments":[{"id":"c1","text":"Ask","author":"user"}]})");
  Versions versions(root);
  const auto first = versions.init();
  auto changes = versions.comment_changes(first);
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].card, "outer-id/inner-id");
  EXPECT_EQ(changes[0].change, "added");
  EXPECT_EQ(changes[0].by, "user");
  Put(
      meta,
      R"({"comments":[{"id":"c1","text":"Ask","author":"user","resolved":true,"state_by":"agent","resolution":"Done","cards":["outer-id/inner-id"]}]})");
  changes = versions.comment_changes(versions.commit_all("Resolve"));
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].change, "resolved");
  EXPECT_EQ(changes[0].by, "agent");
  EXPECT_EQ(changes[0].resolution, "Done");
  EXPECT_EQ(changes[0].cards, (std::vector<std::string>{"outer-id/inner-id"}));
  std::filesystem::remove(root / meta);
  changes = versions.comment_changes(versions.commit_all("Remove"));
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].change, "removed");
  EXPECT_TRUE(changes[0].by.empty());
}
}  // namespace
}  // namespace tsugumi
