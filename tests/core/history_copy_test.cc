#include "history.hh"

#include <gtest/gtest.h>

#include "util/crypto.hh"
#include "wire.hh"

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
    input.status.partial_run = true;
    input.status.uses_partial_result = true;
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
    EXPECT_TRUE(after->status.partial_run);
    EXPECT_TRUE(after->status.uses_partial_result);
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(wire::run_summary(builder, *after));
    const auto* exposed = flatbuffers::GetRoot<Tsugumi::RPC::RunSummary>(builder.GetBufferPointer());
    EXPECT_TRUE(exposed->status()->partial_run());
    EXPECT_TRUE(exposed->status()->uses_partial_result());
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

TEST(HistoryCopy, OlderImportedRunCannotReplaceTheLatestLocalRun) {
  const auto directory = std::filesystem::temp_directory_path() / ("tsugumi-history-" + crypto::uuid4());
  std::filesystem::create_directories(directory);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  History local(directory / "local.sqlite", "secret");
  History elsewhere(directory / "elsewhere.sqlite", "secret");
  RunSummaryData here, imported;
  here.status.run_id = crypto::uuid4();
  here.status.state = RunState::succeeded;
  here.status.accepted_at = 100;
  imported = here;
  imported.status.run_id = crypto::uuid4();
  imported.status.accepted_at = 50;
  local.write_sync([&](sql::Db& db) { History::insert_run(db, here, crypto::uuid4()); });
  elsewhere.write_sync([&](sql::Db& db) { History::insert_run(db, imported, crypto::uuid4()); });
  ASSERT_EQ(elsewhere.export_runs(directory / "copy.sqlite"), 1);
  ASSERT_EQ(local.import_runs(directory / "copy.sqlite"), 1);
  local.read([&](sql::Db& db) {
    const auto latest = History::latest_run(db);
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->status.run_id, here.status.run_id);
    EXPECT_TRUE(History::run(db, *imported.status.run_id));
  });
}

TEST(HistoryCopy, SavedInputProvenanceSurvivesBothSnapshotPathsAndReplacement) {
  const auto directory = std::filesystem::temp_directory_path() / ("tsugumi-history-" + crypto::uuid4());
  std::filesystem::create_directories(directory);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  History history(directory / "history.sqlite", "secret");
  for (const bool partial : {true, false}) {
    SavedInputGroup group{"in", true, {}, partial};
    history.write_sync([&](sql::Db& db) {
      History::put_last_input(db, {"card"}, "run", 1, {group});
      History::put_last_port_input(db, {"card"}, "run", 1, group);
    });
    history.read([&](sql::Db& db) {
      const auto all = History::last_input(db, {"card"});
      const auto port = History::last_port_input(db, {"card"}, "in");
      ASSERT_TRUE(all);
      ASSERT_TRUE(port);
      EXPECT_EQ(all->groups.at(0).partial_result, partial);
      EXPECT_EQ(port->groups.at(0).partial_result, partial);
    });
  }
}

}  // namespace
}  // namespace tsugumi
