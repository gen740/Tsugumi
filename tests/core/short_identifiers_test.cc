#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <set>

#include "history.hh"
#include "host/agent_policy.hh"
#include "host/identifiers.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi {
namespace {
struct IdentifierDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               ("identifiers-" + crypto::uuid4());
  IdentifierDirectory() { std::filesystem::create_directories(path); }
  ~IdentifierDirectory() { std::filesystem::remove_all(path); }
};

TEST(ShortIdentifiers,
     ReservationsRetryForcedCollisionsAndOutliveDeletedObjects) {
  sql::Db db(":memory:");
  db.exec(
      "CREATE TABLE identifiers(scope TEXT, id TEXT, PRIMARY KEY(scope,id))");
  EXPECT_EQ(host::AllocateId(db, "conversation", [] { return "abcdef"; }),
            "abcdef");
  int attempts = 0;
  const auto generate = [&] { return ++attempts < 3 ? "abcdef" : "234567"; };
  EXPECT_EQ(host::AllocateId(db, "conversation", generate), "234567");
  EXPECT_EQ(attempts, 3);
  EXPECT_EQ(host::AllocateId(db, "another-scope", [] { return "abcdef"; }),
            "abcdef");
}

TEST(ShortIdentifiers, OrdinalsCheckOccupiedReferencesAndReserveAcrossClients) {
  sql::Db db(":memory:");
  db.exec(
      "CREATE TABLE identifiers(scope TEXT, id TEXT, PRIMARY KEY(scope,id))");
  EXPECT_EQ(
      host::ReserveOrdinal(db, "project:note", "note-", {"note-1", "note-3"}),
      "note-4");
  EXPECT_EQ(host::ReserveOrdinal(db, "project:note", "note-", {}), "note-5");
  EXPECT_EQ(host::ReserveOrdinal(db, "project:comment:a", "", {}), "1");
  EXPECT_EQ(host::ReserveOrdinal(db, "project:comment:b", "", {}), "1");
  EXPECT_EQ(host::ReserveOrdinal(db, "project:comment:a", "", {"2"}), "3");
}

TEST(ShortIdentifiers, AlphabetAndDirectoryNamesHaveNoCaseAliases) {
  IdentifierDirectory directory;
  std::set<std::string> ids;
  for (int i = 0; i < 256; ++i) {
    const auto id = crypto::unique_short_id(
        [&](const auto &value) { return ids.contains(value); });
    ASSERT_TRUE(crypto::is_short_id(id));
    ASSERT_EQ(id.find_first_of("01iloOIL/+=\\\"' ."), id.npos);
    ASSERT_TRUE(std::all_of(id.begin(), id.end(), [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= '2' && c <= '9');
    }));
    ASSERT_TRUE(std::filesystem::create_directory(directory.path / id));
    fs::write_atomic(directory.path / id / "value", id);
    EXPECT_EQ(fs::read_file_or_throw(directory.path / id / "value"), id);
    ids.insert(id);
  }
  for (const auto *invalid :
       {"", "0", "01", "-1", "../1", "1/2", "9223372036854775808"})
    EXPECT_FALSE(crypto::is_run_id(invalid)) << invalid;
  EXPECT_TRUE(crypto::is_run_id("12"));
}

TEST(ShortIdentifiers,
     RunSequenceSurvivesRestartDeletionAndAnEmptyPortableCopy) {
  IdentifierDirectory directory;
  const auto local = directory.path / "history.sqlite",
             copy = directory.path / "runs.sqlite";
  {
    History history(local, "test-secret");
    history.write_sync([](sql::Db &db) {
      EXPECT_EQ(History::next_run_id(db), "1");
      EXPECT_EQ(History::next_run_id(db), "2");
      db.exec("DELETE FROM runs");
    });
    EXPECT_EQ(history.export_runs(copy), 0u);
  }
  History restarted(local, "test-secret");
  restarted.write_sync(
      [](sql::Db &db) { EXPECT_EQ(History::next_run_id(db), "3"); });
  History imported(directory.path / "imported.sqlite", "test-secret");
  EXPECT_EQ(imported.import_runs(copy), 0u);
  imported.write_sync(
      [](sql::Db &db) { EXPECT_EQ(History::next_run_id(db), "3"); });
}

TEST(ShortIdentifiers, ModelMetadataPreservesIntegrityContentAndGitPrefixes) {
  namespace j = ::tsugumi::json;
  const std::string digest(64, 'a'), commit(40, 'b');
  const auto raw =
      j::Encode(j::Members{{"hash", j::Quote(digest)},
                           {"source_hash", j::Quote(digest)},
                           {"bytes", j::Quote(digest)},
                           {"commit", j::Quote(commit)},
                           {"environment_id", j::Quote("1")},
                           {"run_id", j::Quote("12")},
                           {"artifact_id", j::Quote("12/lorenz/1/image")}});
  const auto result = j::Object(host::AgentReferences(raw));
  EXPECT_EQ(j::Text(result, "hash"), digest);
  EXPECT_EQ(j::Text(result, "source_hash"), digest);
  EXPECT_EQ(j::Text(result, "bytes"), digest);
  EXPECT_EQ(j::Text(result, "commit"), commit.substr(0, 12));
  EXPECT_EQ(j::Text(result, "artifact_id"), "12/lorenz/1/image");
}
} // namespace
} // namespace tsugumi
