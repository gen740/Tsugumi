#include "host/version_text.hh"

#include <gtest/gtest.h>

#include <map>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "version_facts.hh"
#include "version_record.hh"
#include "versions.hh"

namespace tsugumi {
namespace {
template <class T>
auto Record(const std::string& raw) -> T {
  T value;
  if (auto error = schema::decode(raw, value))
    throw std::runtime_error(error->message);
  return value;
}
auto ExpectChanges(
    const std::string& raw, const std::vector<std::string>& added,
    const std::vector<std::string>& removed,
    const std::map<std::string, std::vector<std::string>>& changed) -> void {
  const auto record = Record<version_record::Changes>(raw);
  EXPECT_EQ(*record.added, added);
  EXPECT_EQ(*record.removed, removed);
  std::map<std::string, std::vector<std::string>> found;
  for (const auto& item : *record.changed)
    found.emplace(*item.card, *item.kinds);
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
      R"({"language":"python","params":[{"name":"secret","data":"FIRST"}]})");
  Put("definitions/b/_card.json", "{}");
  Put("definitions/a/src/main.py", std::string(15000, '#') + "\n");
  Versions versions(root);
  const auto first = versions.init();
  ExpectChanges(versions.note(first).record_json, {"a", "b"}, {}, {});
  Put("definitions/a/_card.json",
      R"({"language":"python","params":[{"name":"secret","data":"PRIVATE_SENTINEL"}]})");
  const auto params = versions.commit_all("Parameters");
  const auto neutral = versions.note(params).record_json;
  ExpectChanges(neutral, {}, {}, {{"a", {"parameters"}}});
  EXPECT_EQ(versions.note(params).record_json, neutral);
  EXPECT_EQ(neutral.find("PRIVATE_SENTINEL"), std::string::npos);
  Put("definitions/a/_card.json",
      "{ \"params\": [ { \"data\": \"PRIVATE_SENTINEL\", \"name\": \"secret\" "
      "} ], \"language\": \"python\" "
      "}");
  const auto formatting = versions.commit_all("Formatting");
  ExpectChanges(versions.note(formatting).record_json, {}, {}, {});
  Put("definitions/a/_card.json",
      R"({"language":"python","network":true,"params":[{"name":"secret","data":"PRIVATE_SENTINEL"}]})");
  Put("definitions/a/src/main.py",
      "# CHANGED_SOURCE_SENTINEL\n" + std::string(15000, '#'));
  Put("definitions/c/_card.json", "{}");
  std::filesystem::remove_all(root / "definitions/b");
  Put("tsugumi.json",
      R"({"canvas":{"cards":[{"id":"a"},{"id":"c"}],"connections":[{"from":{"card":"a","port":"out"},"to":{"card":"c","port":"in"}}]}})");
  auto change = versions.commit_all("Change");
  EXPECT_TRUE(host::VersionDiff(root, change).truncated);
  ExpectChanges(versions.note(change).record_json, {"c"}, {"b"},
                {{"a", {"definition", "source", "wiring"}}});
  EXPECT_EQ(host::ChangeText(
                version_facts::changes(versions.note(change).record_json), true)
                .find("SENTINEL"),
            std::string::npos);
  Put("layout.json", R"({"cards":{"a":{"x":42,"y":1}}})");
  Put("definitions/a/data/data.csv", "PRIVATE_DATA_SENTINEL");
  change = versions.commit_all("Layout and data");
  ExpectChanges(versions.note(change).record_json, {}, {}, {});
  EXPECT_TRUE(host::VersionDiff(root, change).text.empty());
  EXPECT_THROW(versions.note(std::string(40, '0')), std::exception);
  Put("definitions/a/src/main.rs", "fn main() {}");
  Put("definitions/a/src/_card.json", "not a Card document");
  change = versions.commit_all("Source inputs");
  ExpectChanges(versions.note(change).record_json, {}, {}, {{"a", {"source"}}});
  EXPECT_TRUE(host::VersionDiff(root, change).text.empty());
}
TEST_F(VersionNote, NestedCardsAndTheEntryLimitUseTheFullDiff) {
  Put("tsugumi.json",
      R"({"canvas":{"cards":[{"id":"inner"}],"connections":[]}})");
  Put("externals/inner/tsugumi.json",
      R"({"canvas":{"cards":[{"id":"a"}],"connections":[]}})");
  Put("externals/inner/definitions/a/_card.json", "{}");
  Versions versions(root);
  versions.init();
  Put("externals/inner/definitions/a/src/main.py", "print('private')");
  auto commit = versions.commit_all("Nested source");
  ExpectChanges(versions.note(commit).record_json, {}, {},
                {{"inner/a", {"source"}}});
  Put("externals/inner/tsugumi.json",
      R"({"canvas":{"cards":[{"id":"a"},{"id":"b"},{"id":"c"},{"id":"d"},{"id":"e"},{"id":"f"},{"id":"g"},{"id":"h"}],"connections":[]}})");
  commit = versions.commit_all("Many Cards");
  ExpectChanges(versions.note(commit).record_json,
                {"inner/b", "inner/c", "inner/d", "inner/e", "inner/f",
                 "inner/g", "inner/h"},
                {}, {});
}
TEST_F(VersionNote, NeutralCommitsAndRunNotesSurviveReopening) {
  Put("tsugumi.json", "{}");
  Versions versions(root);
  const auto commit = versions.init();
  const auto message = versions.graph(1).commits.front().message;
  EXPECT_EQ(*Record<version_record::Commit>(message).kind, "create");
  version_record::Run record;
  *record.whole = true;
  VersionRunNote run{"run-id", "succeeded", "2026-11-15T00:00:00Z", 1.25,
                     schema::encode(record)};
  EXPECT_EQ(*Record<version_record::RunNote>(Versions::format_note(run)).state,
            "succeeded");
  versions.note_run(commit, run);
  Versions reopened(root);
  const auto facts = reopened.graph(1);
  ASSERT_EQ(facts.commits.front().runs.size(), 1u);
  EXPECT_EQ(facts.commits.front().runs.front().seconds, 1.25);
  EXPECT_TRUE(*Record<version_record::Run>(
                   facts.commits.front().runs.front().record_json)
                   .whole);
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
  EXPECT_EQ(host::VersionDiff(root, first).text.find("PRIVATE_DATA"),
            std::string::npos);
  EXPECT_TRUE(host::VersionDiff(root, third, third).text.empty());
  EXPECT_EQ(versions.graph(1).head, third);
  EXPECT_FALSE(versions.graph(1).dirty);
  EXPECT_THROW(host::VersionDiff(root, third, std::string(40, '0')),
               std::exception);
}
TEST(VersionText, PublicFactsSeparatePersistenceFromPresentation) {
  auto commit = version_facts::commit(version_record::commit("run", "card"));
  ASSERT_NE(commit, nullptr);
  EXPECT_EQ(commit->kind, "run");
  EXPECT_FALSE(host::CommitText(*commit, true).empty());
  EXPECT_EQ(version_facts::commit("A human message"), nullptr);
  version_record::Run persisted;
  *persisted.whole = true;
  auto facts = version_facts::run(schema::encode(persisted));
  ASSERT_NE(facts, nullptr);
  EXPECT_TRUE(facts->whole);
  EXPECT_FALSE(host::RunText(*facts, true).empty());
  EXPECT_EQ(version_facts::run(""), nullptr);
  auto changes =
      version_facts::changes(schema::encode(version_record::Changes{}));
  EXPECT_TRUE(changes.added.empty());
  EXPECT_FALSE(host::ChangeText(changes, true).empty());
}
TEST(VersionText, ModelReadPolicyLivesOnTheHost) {
  EXPECT_TRUE(host::DiffReads("externals/p/definitions/a/src/main.py"));
  EXPECT_TRUE(host::DiffReads("definitions/a/_meta.json"));
  EXPECT_FALSE(host::DiffReads("definitions/a/src/data.csv"));
  EXPECT_FALSE(host::DiffReads("definitions/a/data/private.py"));
  EXPECT_FALSE(host::DiffReads("layout.json"));
}
}  // namespace
}  // namespace tsugumi
