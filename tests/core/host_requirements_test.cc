#include <gtest/gtest.h>

#include "host_requirements.hh"
#include "store.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi {
namespace {
class HostRequirements : public ::testing::Test {
 protected:
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               ("host-requirements-" + crypto::uuid4());
  auto SetUp() -> void override {
    fs::write_atomic(root / "tsugumi.json",
        R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"a"}],"connections":[]}})");
    fs::write_atomic(root / "definitions/a/_card.json",
        R"({"hostFolders":[{"name":"data","path":"/data"}]})");
    fs::write_atomic(root / "definitions/unused/_card.json",
        R"({"hostFolders":[{"name":"unused","path":"/unused","write":true}]})");
  }
  auto TearDown() -> void override { std::filesystem::remove_all(root); }
};
TEST_F(HostRequirements, BootstrapAndAcceptedResolutionUseOnlyReachableCards) {
  auto bootstrap = load_host_requirements(root);
  Store store({root, ""});
  store.open();
  auto accepted = host_requirements(*store.current()->resolution, store.current()->rev);
  ASSERT_EQ(accepted.folders.size(), 1u);
  ASSERT_EQ(bootstrap.folders.size(), 1u);
  EXPECT_EQ(accepted.folders[0]->card->segments, (CardPath{"a"}));
  EXPECT_EQ(accepted.folders[0]->path, "/data");
  EXPECT_FALSE(accepted.folders[0]->write);
  EXPECT_EQ(bootstrap.folders[0]->path, accepted.folders[0]->path);
}
TEST_F(HostRequirements, DiskEditsDoNotChangeAcceptedRequirementsUntilReload) {
  Store store({root, ""});
  store.open();
  fs::write_atomic(root / "definitions/a/_card.json",
      R"({"hostFolders":[{"name":"data","path":"/new","write":true}]})");
  auto accepted = host_requirements(*store.current()->resolution);
  ASSERT_EQ(accepted.folders.size(), 1u);
  EXPECT_EQ(accepted.folders[0]->path, "/data");
  EXPECT_FALSE(accepted.folders[0]->write);
  auto staged = store.stage_reload(crypto::uuid4(), "test");
  store.commit(staged, "reload", "digest", [](const Delta*) { return "receipt"; });
  accepted = host_requirements(*store.current()->resolution, store.current()->rev);
  ASSERT_EQ(accepted.folders.size(), 1u);
  EXPECT_EQ(accepted.folders[0]->path, "/new");
  EXPECT_TRUE(accepted.folders[0]->write);
  EXPECT_EQ(accepted.rev, store.current()->rev);
}
TEST_F(HostRequirements, EmptyAndRemoteCardsNeedNoHostShare) {
  fs::write_atomic(root / "definitions/a/_card.json",
      R"({"template":"trigger","hostFolders":[{"name":"unused","path":"/unused"}]})");
  EXPECT_TRUE(load_host_requirements(root).folders.empty());
  fs::write_atomic(root / "definitions/a/_card.json", R"({"executionService":"remote"})");
  EXPECT_TRUE(load_host_requirements(root).folders.empty());
  fs::write_atomic(root / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})");
  EXPECT_TRUE(load_host_requirements(root).folders.empty());
}
}  // namespace
}  // namespace tsugumi
