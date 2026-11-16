#include "host/state_format.hh"

#include <gtest/gtest.h>

#include <future>
#include <vector>

#include "../agent_fixture_server.hh"
#include "util/fs.hh"

namespace tsugumi::test {
TEST(HostStateFormat, FreshFolderIsNumberedAndCanBeReopened) {
  TemporaryDirectory directory;
  const auto state = directory.path / "host-core";
  host::PrepareStateDirectory(state);
  EXPECT_EQ(fs::read_file_or_throw(state / "format"),
            std::to_string(host::kStateFormat) + "\n");
  EXPECT_FALSE(std::filesystem::exists(state / "host.sqlite"));
  EXPECT_NO_THROW(host::PrepareStateDirectory(state));
  // The marker is an integer, independent of a trailing newline.
  fs::write_atomic(state / "format", std::to_string(host::kStateFormat));
  EXPECT_NO_THROW(host::PrepareStateDirectory(state));
}

TEST(HostStateFormat, AnotherFormatIsRefusedWithoutChangingState) {
  TemporaryDirectory directory;
  const auto state = directory.path / "host-core";
  std::filesystem::create_directory(state);
  fs::write_atomic(state / "format", "999999\n");
  fs::write_atomic(state / "host.sqlite", "database must not be opened");
  try {
    host::PrepareStateDirectory(state);
    FAIL() << "Another format must be refused";
  } catch (const host::StateFormatError& error) {
    EXPECT_EQ(error.directory, std::filesystem::absolute(state).lexically_normal());
  }
  EXPECT_EQ(fs::read_file_or_throw(state / "format"), "999999\n");
  EXPECT_EQ(fs::read_file_or_throw(state / "host.sqlite"),
            "database must not be opened");
}

TEST(HostStateFormat, MissingNumberIsRefusedEvenInAnEmptyExistingFolder) {
  TemporaryDirectory directory;
  EXPECT_THROW(host::PrepareStateDirectory(directory.path),
               host::StateFormatError);
  for (const auto* database :
       {"host.sqlite", "host-settings.sqlite", "execution.sqlite"}) {
    fs::write_atomic(directory.path / database, "existing database");
  }
  EXPECT_THROW(host::PrepareStateDirectory(directory.path),
               host::StateFormatError);
  EXPECT_FALSE(std::filesystem::exists(directory.path / "format"));
  for (const auto* database :
       {"host.sqlite", "host-settings.sqlite", "execution.sqlite"}) {
    EXPECT_EQ(fs::read_file_or_throw(directory.path / database),
              "existing database");
  }
}

TEST(HostStateFormat, ConcurrentStartersSeeThePublishedNumber) {
  TemporaryDirectory directory;
  const auto state = directory.path / "host-core";
  std::vector<std::future<void>> starters;
  for (int i = 0; i < 8; ++i) {
    starters.push_back(std::async(
        std::launch::async, [state] { host::PrepareStateDirectory(state); }));
  }
  for (auto& starter : starters) EXPECT_NO_THROW(starter.get());
  EXPECT_EQ(fs::read_file_or_throw(state / "format"),
            std::to_string(host::kStateFormat) + "\n");
}
}  // namespace tsugumi::test
