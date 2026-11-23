#include <git2.h>
#include <gtest/gtest.h>

#include "tsugumi/protocol/runtime_state_generated.h"
#include "tsugumi/util/flat_file.hh"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/document/document.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "versions.hh"

namespace tsugumi {
namespace {
class VersionFiles : public ::testing::Test {
 protected:
  void SetUp() override {
    root = std::filesystem::temp_directory_path() /
           ("tsugumi-version-files-" + crypto::random_token(8));
    std::filesystem::create_directory(root);
  }
  void TearDown() override { std::filesystem::remove_all(root); }
  auto Put(const std::string& path, const std::string& text = "content")
      -> void {
    std::filesystem::create_directories((root / path).parent_path());
    fs::write_atomic(root / path, text, 0644);
  }
  auto Paths(const std::string& commit) -> std::set<std::string> {
    git_repository* repo = nullptr;
    EXPECT_EQ(git_repository_open(&repo, root.c_str()), 0);
    git_object* object = nullptr;
    EXPECT_EQ(git_revparse_single(&object, repo, (commit + "^{tree}").c_str()),
              0);
    std::set<std::string> paths;
    git_tree_walk(
        reinterpret_cast<git_tree*>(object), GIT_TREEWALK_PRE,
        [](const char* prefix, const git_tree_entry* entry, void* payload) {
          if (git_tree_entry_type(entry) == GIT_OBJECT_BLOB)
            static_cast<std::set<std::string>*>(payload)->insert(
                std::string(prefix) + git_tree_entry_name(entry));
          return 0;
        },
        &paths);
    git_object_free(object);
    git_repository_free(repo);
    return paths;
  }
  std::filesystem::path root;
};

TEST_F(VersionFiles, LockDocumentsAreTrackedWithTheirProjects) {
  Put("tsugumi.json", initial_project_json("root"));
  Put("externals/sub/tsugumi.json", initial_project_json("sub"));
  Put("tsugumi.lock", R"({"nixPath":{}})");
  Put("externals/sub/tsugumi.lock", R"({"nixPath":{}})");
  const auto loaded = load_from_disk(root);
  Versions versions(root);
  const auto commit = versions.init();
  const auto paths = Paths(commit);
  EXPECT_TRUE(paths.contains("tsugumi.lock"));
  EXPECT_TRUE(paths.contains("externals/sub/tsugumi.lock"));
  EXPECT_EQ(Versions::card_of("tsugumi.lock"), "");
  EXPECT_EQ(Versions::card_of("externals/sub/tsugumi.lock"), "sub");
  EXPECT_EQ(*loaded->sources.at("tsugumi.lock").document, R"({"nixPath":{}})");
}

TEST_F(VersionFiles, DefinitionsTemplatesAndLiteralOptInsShareOneSelection) {
  Put("tsugumi.json",
      R"({"nixPath": [],"canvas":{"cards":[],"connections":[]},"versionPaths":["inputs/reference.csv","chosen folder","missing","literal[1].csv"]})");
  const std::set<std::string> definitions = {
      "layout.bin",
      "definitions/a/_card.json",
      "definitions/a/README.md",
      "definitions/a/src/main.py",
      "definitions/a/src/image.png",
      "definitions/a/default.nix",
      "definitions/a/flake.lock",
      "definitions/a/_outputs.fbs",
      "definitions/b/_card.json",
      "skills/example/SKILL.md",
      "templates/example/example/_card.json",
      "templates/example/layout.bin",
      "externals/inner/tsugumi.json",
      "externals/inner/definitions/b/_card.json"};
  for (const auto& path : definitions) Put(path);
  Put(".gitignore",
      "*\n");  // Even definitions and explicit choices are ignored by Git.
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
  Put("templates/example/.git/config");
  Put("templates/example/.tsugumi/private");
  std::filesystem::create_directory_symlink(
      root / ".tsugumi", root / "chosen folder/linked-state");
  std::filesystem::create_symlink(root / ".tsugumi/runs.sqlite",
                                  root / "definitions/a/src/linked-state");
  Versions versions(root);
  const auto first = versions.init();
  auto expected = definitions;
  expected.insert({"tsugumi.json", "inputs/reference.csv",
                   "chosen folder/nested/value.csv", "literal[1].csv"});
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

TEST_F(VersionFiles,
       InternalStateAndUnselectedIndexEntriesNeverEnterTheNextVersion) {
  Put("tsugumi.json",
      R"({"nixPath": [],"canvas":{"cards":[],"connections":[]},"versionPaths":["result.csv"]})");
  Put(".gitignore", "# Existing ignore file does not exclude state\n");
  Put("result.csv");
  Put("stray.csv");
  Put(".tsugumi/state");
  Versions versions(root);
  const auto first = versions.init();
  EXPECT_EQ(Paths(first),
            (std::set<std::string>{"tsugumi.json", "result.csv"}));
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
  Put("tsugumi.json",
      R"({"nixPath": [],"canvas":{"cards":[],"connections":[]},"versionPaths":[".tsugumi",".git","externals/inner/.tsugumi"]})");
  const auto next = versions.mark("definitions", "Keep as Version");
  EXPECT_EQ(Paths(next), (std::set<std::string>{"tsugumi.json"}));
  EXPECT_EQ(fs::read_file(root / "result.csv"), "content");
  EXPECT_EQ(fs::read_file(root / "stray.csv"), "content");
  EXPECT_EQ(fs::read_file(root / ".tsugumi/state"), "content");
  EXPECT_EQ(fs::read_file(root / ".gitignore"),
            "# Existing ignore file does not exclude state\n");
  EXPECT_FALSE(versions.graph(10).dirty);
}

TEST_F(VersionFiles, ProjectValidationRejectsEscapesAndInternalPaths) {
  const std::string prefix =
      R"({"name":"p","nixPath": [],"canvas":{"cards":[],"connections":[]},"versionPaths":[)";
  for (const auto& path :
       {"../outside", "/outside", ".", "", "a/../b", "a//b", ".tsugumi",
        "a/.tsugumi/state", ".git/config", ".TSUGUMI/state"}) {
    SCOPED_TRACE(path);
    EXPECT_THROW(parse_project_json(
                     prefix + json::Quote(path) + "]}"),
                 DocumentError);
  }
  EXPECT_NO_THROW(parse_project_json(
      prefix + R"("inputs/reference.csv","chosen folder","missing"]})"));
  EXPECT_THROW(
      parse_project_json(
          R"({"name":"p","nixPath": [],"canvas":{"cards":[],"connections":[]},"versionPaths":"result.csv"})"),
      DocumentError);
}

TEST_F(VersionFiles,
       ImportedDemoDifferencesCannotAddInternalOrUnselectedFiles) {
  Put("tsugumi.json",
      R"({"nixPath": [],"canvas":{"cards":[],"connections":[]},"versionPaths":["chosen.csv"]})");
  Put("definitions/a/_card.json", R"({"type":"code"})");
  Put(".tsugumi/state");
  Put("result.csv");
  using namespace Tsugumi::State;
  HistoryT history;
  auto first = std::make_unique<HistoryCommitT>();
  first->id = "first";
  first->message = "First";
  first->time = 1793700000;
  for (const auto& [path, bytes] : std::map<std::string, std::string>{
           {"chosen.csv", "chosen"}, {".tsugumi/state", "private"},
           {"externals/inner/.tsugumi/state", "private"}, {"result.csv", "result"},
           {"definitions/a/_card.json", R"({"type":"draft"})"}}) {
    auto file = std::make_unique<HistoryFileT>();
    file->path = path;
    file->bytes.assign(bytes.begin(), bytes.end());
    first->files.push_back(std::move(file));
  }
  history.commits.push_back(std::move(first));
  auto main = std::make_unique<HistoryBranchT>();
  main->name = "main";
  main->commit = "first";
  history.branches.push_back(std::move(main));
  history.head = "main";
  Put(".tsugumi-history", flat::encode<Tsugumi::State::History>(history));
  Versions versions(root);
  EXPECT_EQ(Paths(versions.init()),
            (std::set<std::string>{"tsugumi.json", "definitions/a/_card.json",
                                   "chosen.csv"}));
  EXPECT_EQ(fs::read_file(root / ".tsugumi/state"), "content");
  EXPECT_EQ(fs::read_file(root / "result.csv"), "content");
}
}  // namespace
}  // namespace tsugumi
