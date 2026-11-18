#include "tsugumi/runtime_support/cache_usage.hh"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>

#include "run_scratch.hh"
#include "tsugumi/util/crypto.hh"

namespace tsugumi {
namespace {

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

TEST_F(RunScratch, ValidatingAnUnopenedProjectDoesNotCreateItsScratch) {
  const auto root = base / "project";
  EXPECT_EQ(disk_scratch_path(root / "runs"), root / "runs");
  EXPECT_FALSE(std::filesystem::exists(root));
  EXPECT_EQ(private_disk_scratch(root / "runs"), root / "runs");
  EXPECT_TRUE(std::filesystem::is_directory(root / "runs"));
}

TEST_F(RunScratch, IsPrivateAndDoesNotFollowLinks) {
  const auto directory = private_disk_scratch(base / "project");
  struct stat st{};
  ASSERT_EQ(::stat(directory.c_str(), &st), 0);
  EXPECT_EQ(st.st_uid, ::geteuid());
  EXPECT_EQ(st.st_mode & 0777, 0700);
  std::filesystem::create_directory_symlink(directory, base / "link");
  EXPECT_THROW(private_disk_scratch(base / "link" / "child"),
               std::runtime_error);
  EXPECT_THROW(disk_scratch_path(base / "link" / "child"), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(directory / "child"));
}

TEST_F(RunScratch, RefusesAParentWritableByOtherUsers) {
  std::filesystem::create_directory(base / "public");
  ASSERT_EQ(::chmod((base / "public").c_str(), 0777), 0);
  EXPECT_THROW(private_disk_scratch(base / "public" / "project"),
               std::runtime_error);
}

TEST_F(RunScratch, MeasurementsRetainAllRunsAndDeletionDoesNotFollowLinks) {
  const auto old = base / "runs" / crypto::uuid4();
  const auto latest = base / "runs" / crypto::uuid4();
  std::filesystem::create_directories(old / "work");
  std::filesystem::create_directories(latest);
  std::ofstream(old / "work" / "diagnostic") << "diagnostic";
  std::ofstream(latest / "output") << "latest";
  const auto outside = base / "keep";
  std::filesystem::create_directory(outside);
  std::ofstream(outside / "evidence") << "do not delete";
  std::filesystem::create_directory_symlink(outside, old / "link");
  std::filesystem::create_directory_symlink(outside, base / "runs" / "link");
  const auto measured = measure_run_scratch(base);
  ASSERT_EQ(measured.size(), 3);
  uint64_t bytes = 0;
  for (const auto& entry : measured) bytes += entry.bytes.value();
  EXPECT_EQ(bytes, 16);
  EXPECT_EQ(measure_run_scratch(base).size(), 3);
  EXPECT_TRUE(std::filesystem::exists(old / "work" / "diagnostic"));
  std::filesystem::permissions(
      old / "work",
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
  EXPECT_TRUE(remove_run_scratch(old));
  EXPECT_FALSE(std::filesystem::exists(old));
  EXPECT_TRUE(std::filesystem::exists(latest / "output"));
  EXPECT_TRUE(std::filesystem::exists(outside / "evidence"));
  EXPECT_TRUE(remove_run_scratch(base / "runs" / "link"));
  EXPECT_TRUE(std::filesystem::exists(outside / "evidence"));
  EXPECT_TRUE(
      remove_run_scratch(old));  // retry after a partial operation is harmless
}

// Stopping the service joins the usage thread. A walk that cannot be
// interrupted makes the stop last as long as every project's scratch.
TEST_F(RunScratch, ACancelledMeasurementStopsWalking) {
  for (int run = 0; run < 8; ++run) {
    const auto folder = base / "runs" / crypto::uuid4();
    std::filesystem::create_directories(folder);
    for (int file = 0; file < 8; ++file)
      std::ofstream(folder / std::to_string(file)) << "value";
  }
  int asked = 0;
  EXPECT_EQ(measure_run_scratch(base, [&] { return ++asked, false; }).size(),
            8);
  const int entries = asked;
  EXPECT_GE(entries, 8 * 8);
  asked = 0;
  const auto partial = measure_run_scratch(base, [&] { return ++asked > 3; });
  EXPECT_EQ(asked,
            4);  // Never asked again, and nothing read, after the answer.
  EXPECT_LT(partial.size(), 8);
}

TEST_F(RunScratch, UnreadableFolderStaysListedAndCanBeRemoved) {
  if (::geteuid() == 0)
    GTEST_SKIP() << "Permission denial requires a non-root user";
  const auto locked = base / "runs" / crypto::uuid4();
  const auto other = base / "runs" / crypto::uuid4();
  std::filesystem::create_directories(locked / "private");
  std::filesystem::create_directories(other);
  std::ofstream(locked / "private" / "value") << "unknown";
  std::ofstream(other / "value") << "known";
  ASSERT_EQ(::chmod((locked / "private").c_str(), 0000), 0);
  const auto measured = measure_run_scratch(base);
  ASSERT_EQ(measured.size(), 2);
  for (const auto& entry : measured) {
    if (entry.path == locked.lexically_relative(base))
      EXPECT_FALSE(entry.bytes);
    else
      EXPECT_EQ(entry.bytes, 5);
  }
  EXPECT_TRUE(remove_run_scratch(other));
  EXPECT_TRUE(remove_run_scratch(locked));
  EXPECT_TRUE(measure_run_scratch(base).empty());
}

#ifdef __linux__
TEST_F(RunScratch, RefusesTmpfsForLargeOutputs) {
  if (!std::filesystem::is_directory("/dev/shm")) GTEST_SKIP();
  const auto path = std::filesystem::path("/dev/shm") /
                    ("tsugumi-" + crypto::random_token(8));
  EXPECT_THROW(private_disk_scratch(path), std::runtime_error);
  std::filesystem::remove_all(path);
}
#endif

}  // namespace
}  // namespace tsugumi
