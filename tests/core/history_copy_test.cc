#include "history.hh"

#include <gtest/gtest.h>

#include "util/crypto.hh"

namespace tsugumi {
namespace {

TEST(HistoryCopy, RoundTripPreservesEveryRunFieldIncludingCommit) {
  const auto directory = std::filesystem::temp_directory_path() / ("tsugumi-history-" + crypto::uuid4());
  std::filesystem::create_directories(directory);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  History original(directory / "original.sqlite", "secret");
  History restored(directory / "restored.sqlite", "secret");
  for (const auto state : {RunState::succeeded, RunState::failed, RunState::cancelled}) {
    RunSummaryData input;
    input.status.run_id = crypto::uuid4();
    input.status.state = state;
    input.status.rev = 17;
    input.status.execution_rev = 13;
    input.status.accepted_at = 100;
    input.status.started_at = 200;
    input.status.ended_at = 300;
    input.status.commit = "a1234567890123456789012345678901234567890";
    if (state != RunState::succeeded) {
      input.status.failure = FailureData::make(FailureKind::cancelled, "diagnostic", {{"reason", "test"}});
      input.status.failure->current_rev = 19;
      input.status.failure->retry_after_ms = 50;
    }
    input.source_hash = "source fingerprint";
    input.target = {"scope", "card"};
    input.propagate = false;
    const auto op_id = crypto::uuid4();
    original.write_sync([&](sql::Db& db) {
      History::insert_run(db, input, op_id);
      db.prepare("UPDATE runs SET has_artifacts=1 WHERE run_id=?").bind(1, *input.status.run_id).run();
    });
    ASSERT_EQ(original.export_runs(directory / "runs.sqlite"), 1);
    ASSERT_EQ(restored.import_runs(directory / "runs.sqlite"), 1);
    const auto before = original.read([&](sql::Db& db) { return History::run(db, *input.status.run_id); });
    const auto after = restored.read([&](sql::Db& db) { return History::run(db, *input.status.run_id); });
    ASSERT_TRUE(before);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->status.run_id, before->status.run_id);
    EXPECT_EQ(after->status.state, before->status.state);
    EXPECT_EQ(after->status.rev, before->status.rev);
    EXPECT_EQ(after->status.execution_rev, before->status.execution_rev);
    EXPECT_EQ(after->status.accepted_at, before->status.accepted_at);
    EXPECT_EQ(after->status.started_at, before->status.started_at);
    EXPECT_EQ(after->status.ended_at, before->status.ended_at);
    EXPECT_EQ(after->status.commit, before->status.commit);
    ASSERT_EQ(after->status.failure.has_value(), before->status.failure.has_value());
    if (before->status.failure) {
      EXPECT_EQ(after->status.failure->kind, before->status.failure->kind);
      EXPECT_EQ(after->status.failure->args, before->status.failure->args);
      EXPECT_EQ(after->status.failure->diagnostic, before->status.failure->diagnostic);
      EXPECT_EQ(after->status.failure->current_rev, before->status.failure->current_rev);
      EXPECT_EQ(after->status.failure->retry_after_ms, before->status.failure->retry_after_ms);
    }
    EXPECT_EQ(after->source_hash, before->source_hash);
    EXPECT_EQ(after->target, before->target);
    EXPECT_EQ(after->propagate, before->propagate);
    restored.read([&](sql::Db& db) {
      auto st = db.prepare("SELECT op_id, has_artifacts FROM runs WHERE run_id=?");
      st.bind(1, *input.status.run_id);
      ASSERT_TRUE(st.step());
      EXPECT_EQ(st.text(0), op_id);
      EXPECT_EQ(st.i64(1), 1);
    });
  }
  EXPECT_EQ(restored.import_runs(directory / "runs.sqlite"), 0);
}

}  // namespace
}  // namespace tsugumi
