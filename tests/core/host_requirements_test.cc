#include "tsugumi/document/host_requirements.hh"

#include <gtest/gtest.h>

#include "store.hh"
#include "test_document.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi {
namespace {
class HostRequirements : public ::testing::Test {
 protected:
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               ("host-requirements-" + crypto::uuid4());
  auto SetUp() -> void override {
    fs::write_atomic(
        root / "tsugumi.json",
        test::project_json(
            R"({"nixPath": [],"hostPaths":[{"name":"data","description":"Input data"}],"canvas":{"cards":[{"id":"a"}],"connections":[]}})"));
    test::write_card(
        root / "definitions/a/_card.json",
        test::card_json(
            R"({"type":"code","grants":{"gui":false,"network":false,"hostPaths":[{"name":"data","write":false}]}})"));
  }
  auto TearDown() -> void override { std::filesystem::remove_all(root); }
};
TEST_F(HostRequirements, BootstrapAndAcceptedResolutionUseOnlyReachableCards) {
  auto bootstrap = load_host_requirements(root);
  Store store({root, ""});
  store.open();
  auto accepted =
      host_requirements(*store.current()->resolution, store.current()->rev);
  ASSERT_EQ(accepted.folders.size(), 1u);
  ASSERT_EQ(bootstrap.folders.size(), 1u);
  EXPECT_EQ(accepted.folders[0]->card->segments, (CardPath{"a"}));
  EXPECT_EQ(accepted.folders[0]->name, "data");
  EXPECT_FALSE(accepted.folders[0]->write);
  EXPECT_EQ(bootstrap.folders[0]->name, accepted.folders[0]->name);
}
TEST_F(HostRequirements, DiskEditsDoNotChangeAcceptedRequirementsUntilReload) {
  Store store({root, ""});
  store.open();
  test::write_card(
      root / "definitions/a/_card.json",
      test::card_json(
          R"({"type":"code","grants":{"gui":false,"network":false,"hostPaths":[{"name":"data","write":true}]}})"));
  auto accepted = host_requirements(*store.current()->resolution);
  ASSERT_EQ(accepted.folders.size(), 1u);
  EXPECT_EQ(accepted.folders[0]->name, "data");
  EXPECT_FALSE(accepted.folders[0]->write);
  auto staged = store.stage_reload(crypto::uuid4(), "test");
  store.commit(staged, "reload", "digest",
               [](const Delta*) { return "receipt"; });
  accepted =
      host_requirements(*store.current()->resolution, store.current()->rev);
  ASSERT_EQ(accepted.folders.size(), 1u);
  EXPECT_EQ(accepted.folders[0]->name, "data");
  EXPECT_TRUE(accepted.folders[0]->write);
  EXPECT_EQ(accepted.rev, store.current()->rev);
}
TEST_F(HostRequirements, UnknownNamesCannotBeLoaded) {
  test::write_card(
      root / "definitions/a/_card.json",
      test::card_json(
          R"({"type":"code","grants":{"gui":false,"network":false,"hostPaths":[{"name":"unknown","write":false}]}})"));
  try {
    load_host_requirements(root);
    FAIL() << "An undeclared host path was accepted";
  } catch (const DocumentError& error) {
    EXPECT_NE(std::string(error.what()).find("unknown host path 'unknown'"),
              std::string::npos);
  }
}
TEST_F(HostRequirements, CardsWithoutHostPathsNeedNoHostShare) {
  test::write_card(root / "definitions/a/_card.json",
                   test::card_json(R"({"type":"trigger"})"));
  EXPECT_TRUE(load_host_requirements(root).folders.empty());
  test::write_card(
      root / "definitions/a/_card.json",
      test::card_json(R"({"type":"code","workerCapabilities":[1]})"));
  EXPECT_TRUE(load_host_requirements(root).folders.empty());
}
}  // namespace
}  // namespace tsugumi
