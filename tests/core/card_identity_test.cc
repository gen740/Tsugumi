#include "store.hh"

#include <gtest/gtest.h>

#include <regex>
#include <set>

#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi {
namespace {

TEST(CardIdentity, MetadataIsNonComputationalOnlyAtDefinitionPaths) {
  EXPECT_TRUE(Document::is_card_meta_file("definitions/card/_meta.json"));
  EXPECT_TRUE(Document::is_card_meta_file("externals/outer/externals/inner/definitions/card/_meta.json"));
  EXPECT_FALSE(Document::is_card_meta_file("definitions/card/src/definitions/data/_meta.json"));
  EXPECT_FALSE(Document::is_card_meta_file("externals/outer/definitions/card/_card.json"));
}

TEST(CardIdentity, CoreReservesUniqueReadableIdentifiersAcrossRestartsAndScopes) {
  auto root = std::filesystem::temp_directory_path() / ("card-identity-" + crypto::uuid4());
  std::filesystem::create_directories(root);
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::filesystem::remove_all(root); }
  } cleanup{root};
  fs::write_atomic(root / "tsugumi.json", R"({"name":"test","pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})");
  fs::write_atomic(root / "layout.json", "{}");
  fs::write_atomic(root / "externals/child/tsugumi.json", R"({"canvas":{"cards":[],"connections":[]}})");
  std::set<std::string> ids;
  std::regex shape("aggregate-[abcdefghjkmnpqrstuvwxyz23456789]{4}");
  {
    Store store({root, ""});
    store.open();
    auto rev = store.current()->rev;
    for (int i = 0; i < 100; ++i) {
      auto id = store.allocate_card_id("aggregate", "");
      EXPECT_TRUE(std::regex_match(id, shape));
      EXPECT_TRUE(ids.insert(id).second);
    }
    EXPECT_EQ(store.current()->rev, rev);
    EXPECT_THROW(store.allocate_card_id("A", "../"), Failed);
    EXPECT_THROW(store.allocate_card_id("A", "externals/../"), Failed);
    EXPECT_THROW(store.allocate_card_id("", ""), Failed);
    EXPECT_THROW(store.allocate_card_id(std::string(124, 'a'), ""), Failed);
    EXPECT_EQ(store.allocate_card_id(std::string(123, 'a'), "").size(), 128u);
    EXPECT_THROW(store.allocate_card_id("集計", ""), Failed);
    EXPECT_TRUE(store.allocate_card_id("Already_Chosen", "").starts_with("Already_Chosen-"));
  }
  Store store({root, ""});
  store.open();
  EXPECT_TRUE(ids.insert(store.allocate_card_id("aggregate", "")).second);
  EXPECT_TRUE(std::regex_match(store.allocate_card_id("aggregate", "externals/child/"), shape));
  EXPECT_TRUE(std::regex_match(store.allocate_card_id("aggregate", "externals/child/externals/inner/"), shape));
  auto reservations = fs::read_file(root / ".tsugumi/card-ids").value();
  for (const auto& id : ids) EXPECT_NE(reservations.find(id + '\n'), std::string::npos);
}

}  // namespace
}  // namespace tsugumi
