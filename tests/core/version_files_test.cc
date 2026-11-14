#include "versions.hh"

#include <git2.h>
#include <gtest/gtest.h>

#include "document.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi {
namespace {
class VersionFiles : public ::testing::Test {
 protected:
  void SetUp() override {
    root = std::filesystem::temp_directory_path() / ("tsugumi-version-files-" + crypto::random_token(8));
    std::filesystem::create_directory(root);
  }
  void TearDown() override { std::filesystem::remove_all(root); }
  auto Put(const std::string& path, const std::string& text = "content") -> void {
    std::filesystem::create_directories((root / path).parent_path());
    fs::write_atomic(root / path, text, 0644);
  }
  auto Paths(const std::string& commit) -> std::set<std::string> {
    git_repository* repo = nullptr;
    EXPECT_EQ(git_repository_open(&repo, root.c_str()), 0);
    git_object* object = nullptr;
    EXPECT_EQ(git_revparse_single(&object, repo, (commit + "^{tree}").c_str()), 0);
    std::set<std::string> paths;
    git_tree_walk(reinterpret_cast<git_tree*>(object), GIT_TREEWALK_PRE,
                  [](const char* prefix, const git_tree_entry* entry, void* payload) {
                    if (git_tree_entry_type(entry) == GIT_OBJECT_BLOB)
                      static_cast<std::set<std::string>*>(payload)->insert(std::string(prefix) + git_tree_entry_name(entry));
                    return 0;
                  }, &paths);
    git_object_free(object);
    git_repository_free(repo);
    return paths;
  }
  std::filesystem::path root;
};

TEST_F(VersionFiles, DefinitionsAndLiteralOptInsShareOneSelection) {
  Put("tsugumi.json", R"({"versionPaths":["inputs/reference.csv","chosen folder","missing","literal[1].csv"]})");
  const std::set<std::string> definitions = {
      "layout.json", "definitions/a/_card.json", "definitions/a/_meta.json", "definitions/a/src/main.py",
      "definitions/a/src/image.png", "definitions/a/default.nix", "definitions/a/flake.lock",
      "definitions/a/_outputs.fbs", "definitions/b/_link.json", "skills/example/SKILL.md",
      "externals/inner/tsugumi.json", "externals/inner/definitions/b/_card.json"};
  for (const auto& path : definitions) Put(path);
  Put(".gitignore", "*\n");  // Even definitions and explicit choices are ignored by Git.
  Put("inputs/reference.csv");
  Put("inputs/unselected.csv");
  Put("chosen folder/nested/value.csv");
  Put("chosen folder/.tsugumi/state");
  Put("chosen folder/.git/config");
  Put("literal[1].csv");
  Put("literal1.csv");
  Put("result.csv");
  Put(".tsugumi/runs.sqlite");
  Put("externals/inner/.tsugumi/runs.sqlite");
  Put("definitions/a/.tsugumi/private");
  std::filesystem::create_directory_symlink(root / ".tsugumi", root / "chosen folder/linked-state");
  std::filesystem::create_symlink(root / ".tsugumi/runs.sqlite", root / "definitions/a/src/linked-state");
  Versions versions(root);
  const auto first = versions.init(true);
  auto expected = definitions;
  expected.insert({"tsugumi.json", "inputs/reference.csv", "chosen folder/nested/value.csv", "literal[1].csv"});
  EXPECT_EQ(Paths(first), expected);
  EXPECT_FALSE(versions.graph(10).dirty);
  Put("result.csv", "stray change");
  EXPECT_EQ(versions.commit_all("Run"), first);
  EXPECT_FALSE(versions.graph(10).dirty);
  Put("definitions/a/src/main.py", "definition change");
  EXPECT_TRUE(versions.graph(10).dirty);
  const auto second = versions.commit_all("Run");
  EXPECT_NE(second, first);
  Put("inputs/reference.csv", "chosen change");
  const auto third = versions.mark("chosen", "Keep as Version");
  EXPECT_NE(third, second);
  EXPECT_EQ(Paths(third), expected);
  std::filesystem::remove(root / "definitions/a/src/main.py");
  const auto fourth = versions.commit_all("Delete a definition source");
  expected.erase("definitions/a/src/main.py");
  EXPECT_EQ(Paths(fourth), expected);
  EXPECT_EQ(fs::read_file(root / "result.csv"), "stray change");
  EXPECT_EQ(fs::read_file(root / ".tsugumi/runs.sqlite"), "content");
}

TEST_F(VersionFiles, InternalStateAndUnselectedIndexEntriesNeverEnterTheNextVersion) {
  Put("tsugumi.json", R"({"versionPaths":["result.csv"]})");
  Put(".gitignore", "# Existing ignore file does not exclude state\n");
  Put("result.csv");
  Put("stray.csv");
  Put(".tsugumi/state");
  Versions versions(root);
  const auto first = versions.init(true);
  EXPECT_EQ(Paths(first), (std::set<std::string>{"tsugumi.json", "result.csv"}));
  git_repository* repo = nullptr;
  ASSERT_EQ(git_repository_open(&repo, root.c_str()), 0);
  git_index* index = nullptr;
  ASSERT_EQ(git_repository_index(&index, repo), 0);
  ASSERT_EQ(git_index_add_bypath(index, "stray.csv"), 0);
  ASSERT_EQ(git_index_add_bypath(index, ".tsugumi/state"), 0);
  ASSERT_EQ(git_index_write(index), 0);
  git_index_free(index);
  git_repository_free(repo);
  // Also defend direct callers against an unvalidated internal-state opt-in.
  Put("tsugumi.json", R"({"versionPaths":[".tsugumi",".git","externals/inner/.tsugumi"]})");
  const auto next = versions.mark("definitions", "Keep as Version");
  EXPECT_EQ(Paths(next), (std::set<std::string>{"tsugumi.json"}));
  EXPECT_EQ(fs::read_file(root / "result.csv"), "content");
  EXPECT_EQ(fs::read_file(root / "stray.csv"), "content");
  EXPECT_EQ(fs::read_file(root / ".tsugumi/state"), "content");
  EXPECT_EQ(fs::read_file(root / ".gitignore"), "# Existing ignore file does not exclude state\n");
  EXPECT_FALSE(versions.graph(10).dirty);
}

TEST_F(VersionFiles, ProjectValidationRejectsEscapesAndInternalPaths) {
  const std::string prefix = R"({"name":"p","pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]},"versionPaths":[)";
  for (const auto& path : {"../outside", "/outside", ".", "", "a/../b", "a//b", ".tsugumi", "a/.tsugumi/state", ".git/config", ".TSUGUMI/state"}) {
    SCOPED_TRACE(path);
    EXPECT_THROW(parse_project_json(prefix + schema::text(schema::string_value(path)) + "]}"), DocumentError);
  }
  EXPECT_NO_THROW(parse_project_json(prefix + R"("inputs/reference.csv","chosen folder","missing"]})"));
  EXPECT_THROW(parse_project_json(R"({"name":"p","pkgsPath":"x","canvas":{"cards":[],"connections":[]},"versionPaths":"result.csv"})"), DocumentError);
}

TEST_F(VersionFiles, ImportedDemoDifferencesCannotAddInternalOrUnselectedFiles) {
  Put("tsugumi.json", R"({"versionPaths":["chosen.csv"]})");
  Put("definitions/a/_card.json", "{}");
  Put(".tsugumi/state");
  Put("result.csv");
  Put(".tsugumi-history.json", R"({"commits":[{"id":"first","message":"First","time":1793700000,"files":{
    "chosen.csv":"chosen", ".tsugumi/state":"private", "externals/inner/.tsugumi/state":"private",
    "result.csv":"result", "definitions/a/_card.json":"{\"template\":\"draft\"}"
  }}],"branches":{"main":"first"},"head":"main"})");
  Versions versions(root);
  EXPECT_EQ(Paths(versions.init(true)), (std::set<std::string>{"tsugumi.json", "definitions/a/_card.json", "chosen.csv"}));
  EXPECT_EQ(fs::read_file(root / ".tsugumi/state"), "content");
  EXPECT_EQ(fs::read_file(root / "result.csv"), "content");
}
}  // namespace
}  // namespace tsugumi
