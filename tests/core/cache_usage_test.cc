#include "cache_usage.hh"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include "run_scratch.hh"
#include "util/crypto.hh"

namespace tsugumi {
namespace {

TEST(CacheNotice, RequiresBothOneGiBAndOneFifthOfTheDisk) {
  constexpr uint64_t gib = uint64_t{1} << 30;
  EXPECT_FALSE(cache_is_large(gib - 1, 2 * gib));
  EXPECT_TRUE(cache_is_large(gib, 2 * gib));
  EXPECT_FALSE(cache_is_large(20 * gib - 1, 100 * gib));
  EXPECT_TRUE(cache_is_large(20 * gib, 100 * gib));
  EXPECT_FALSE(cache_is_large(UINT64_MAX, 0));
  EXPECT_EQ(cache_notice_threshold(UINT64_MAX), UINT64_MAX / 5);
}

class RunScratch : public ::testing::Test {
 protected:
  void SetUp() override {
    // Canonicalize macOS's /var and /tmp aliases before checking parents.
    base = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
           ("tsugumi-scratch-test-" + crypto::random_token(8));
    std::filesystem::create_directory(base);
  }
  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(base, error);
  }
  std::filesystem::path base;
};

TEST_F(RunScratch, IsPrivateAndDoesNotFollowLinks) {
  const auto directory = private_disk_scratch(base / "project");
  struct stat st{};
  ASSERT_EQ(::stat(directory.c_str(), &st), 0);
  EXPECT_EQ(st.st_uid, ::geteuid());
  EXPECT_EQ(st.st_mode & 0777, 0700);
  std::filesystem::create_directory_symlink(directory, base / "link");
  EXPECT_THROW(private_disk_scratch(base / "link" / "child"), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(directory / "child"));
}

TEST_F(RunScratch, RefusesAParentWritableByOtherUsers) {
  std::filesystem::create_directory(base / "public");
  ASSERT_EQ(::chmod((base / "public").c_str(), 0777), 0);
  EXPECT_THROW(private_disk_scratch(base / "public" / "project"), std::runtime_error);
}

#ifdef __linux__
TEST_F(RunScratch, RefusesTmpfsForLargeOutputs) {
  if (!std::filesystem::is_directory("/dev/shm")) GTEST_SKIP();
  const auto path = std::filesystem::path("/dev/shm") / ("tsugumi-" + crypto::random_token(8));
  EXPECT_THROW(private_disk_scratch(path), std::runtime_error);
  std::filesystem::remove_all(path);
}
#endif

}  // namespace
}  // namespace tsugumi
