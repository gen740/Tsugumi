#include <gtest/gtest.h>

#include <regex>
#include <set>

#include "service.hh"
#include "store.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi {
namespace {

TEST(InitialDocument, LoadsAnEmptyProjectAndEscapesItsName) {
  const auto root =
      std::filesystem::temp_directory_path() / ("initial-" + crypto::uuid4());
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::filesystem::remove_all(root); }
  } cleanup{root};
  const std::string name = "New \"project\"\n日本語";
  const auto text = initial_project_json(name);
  const auto project = parse_project_json(text);
  EXPECT_EQ(schema::text(project.name->value()),
            schema::text(schema::string_value(name)));
  EXPECT_TRUE(project.canvas->value().cards->value().empty());
  EXPECT_TRUE(project.canvas->value().connections->value().empty());
  fs::write_atomic(root / "tsugumi.json", text);
  const auto loaded = load_from_disk(root);
  EXPECT_EQ(loaded->project_json, text);
  EXPECT_TRUE(loaded->definitions.empty());
  check_structure(*loaded);
}

TEST(CardIdentity, MetadataIsNonComputationalOnlyAtDefinitionPaths) {
  EXPECT_TRUE(Document::is_card_meta_file("definitions/card/_meta.json"));
  EXPECT_TRUE(Document::is_card_meta_file(
      "externals/outer/externals/inner/definitions/card/_meta.json"));
  EXPECT_FALSE(Document::is_card_meta_file(
      "definitions/card/src/definitions/data/_meta.json"));
  EXPECT_FALSE(Document::is_card_meta_file(
      "externals/outer/definitions/card/_card.json"));
}

TEST(CoreLifecycle, NeverRunProjectDoesNotCreateRunScratch) {
  const auto root =
      std::filesystem::canonical(std::filesystem::temp_directory_path()) /
      ("wired-draft-" + crypto::uuid4());
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::filesystem::remove_all(root); }
  } cleanup{root};
  fs::write_atomic(root / "tsugumi.json", initial_project_json("test"));
  fs::write_atomic(root / "substituters", "");
  ServiceOptions options;
  options.root = root;
  options.scratch_dir = root / "scratch";
  options.run_scratch_dir = root / "run-scratch";
  options.usage_file = root / "usage";
  options.substituters_file = root / "substituters";
  Service service(options);
  Server server({}, &service);
  service.attach(&server);
  service.open();
  EXPECT_FALSE(std::filesystem::exists(options.run_scratch_dir));

  service.shutdown();
  EXPECT_FALSE(std::filesystem::exists(options.run_scratch_dir));
}

TEST(CardIdentity,
     CoreReservesUniqueReadableIdentifiersAcrossRestartsAndScopes) {
  auto root = std::filesystem::temp_directory_path() /
              ("card-identity-" + crypto::uuid4());
  std::filesystem::create_directories(root);
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::filesystem::remove_all(root); }
  } cleanup{root};
  fs::write_atomic(root / "tsugumi.json", initial_project_json("test"));
  fs::write_atomic(root / "layout.json", "{}");
  fs::write_atomic(root / "externals/child/tsugumi.json",
                   R"({"canvas":{"cards":[],"connections":[]}})");
  fs::write_atomic(root / ".tsugumi/card-ids", "CASE\n");
  std::set<std::string> ids;
  std::regex shape("aggregate(?:-[1-9][0-9]*)?");
  {
    Store store({root, ""});
    store.open();
    auto rev = store.current()->rev;
    for (int i = 0; i < 2; ++i) {
      auto id = store.allocate_card_id("aggregate", "");
      EXPECT_TRUE(std::regex_match(id, shape));
      EXPECT_TRUE(ids.insert(id).second);
    }
    EXPECT_EQ(store.current()->rev, rev);
    EXPECT_THROW(store.allocate_card_id("A", "../"), Failed);
    EXPECT_THROW(store.allocate_card_id("A", "externals/../"), Failed);
    EXPECT_THROW(store.allocate_card_id("", ""), Failed);
    EXPECT_THROW(store.allocate_card_id(std::string(124, 'a'), ""), Failed);
    EXPECT_EQ(store.allocate_card_id(std::string(123, 'a'), "").size(), 123u);
    EXPECT_THROW(store.allocate_card_id("集計", ""), Failed);
    EXPECT_EQ(store.allocate_card_id("CASE", ""), "case-2");
    EXPECT_EQ(store.allocate_card_id("Already_Chosen", ""), "already_chosen");
    EXPECT_EQ(store.allocate_card_id("already_chosen", ""), "already_chosen-2");
    fs::write_atomic(root / "definitions/occupied/_card.json",
                     R"({"type":"draft"})");
    EXPECT_EQ(store.allocate_card_id("occupied", ""), "occupied-2");
  }
  Store store({root, ""});
  store.open();
  EXPECT_TRUE(ids.insert(store.allocate_card_id("aggregate", "")).second);
  EXPECT_TRUE(std::regex_match(
      store.allocate_card_id("aggregate", "externals/child/"), shape));
  EXPECT_TRUE(std::regex_match(
      store.allocate_card_id("aggregate", "externals/child/externals/inner/"),
      shape));
}

}  // namespace
}  // namespace tsugumi
