#include "history.hh"

#include <gtest/gtest.h>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "wire.hh"

namespace tsugumi {
namespace {

TEST(RunHistory, NotebookFailurePositionRoundtrip) {
  auto failure = FailureData::make(FailureKind::worker_failed,
                                   "calculation: ZeroDivisionError\nTraceback");
  failure.args = {
      {"source", "src/notebook.py"}, {"line", "8"}, {"cell", "calculation"}};
  const auto restored = FailureData::from_bytes(failure.to_bytes());
  ASSERT_TRUE(restored);
  EXPECT_EQ(restored->args, failure.args);
  EXPECT_EQ(restored->diagnostic, failure.diagnostic);
  EXPECT_EQ(restored->kind, failure.kind);
}

TEST(RunHistory, ArtifactRowsAndSavedInputSchemaSurviveDatabaseRestart) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("tsugumi-history-" + crypto::uuid4());
  std::filesystem::create_directories(directory);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  const auto database = directory / "runs.sqlite";
  const ArtifactKey id(16, '\0');
  const std::string bytes("\0\xff\x80\xc3(\n", 6);
  const Value value{"value", nullptr, bytes, "schema-id"};
  {
    History original(database);
    RunSummaryData run;
    run.status.run_id = "1";
    run.status.state = RunState::succeeded;
    InvocationRecordData record;
    record.summary.run_id = "1";
    record.summary.card = {"card"};
    record.summary.invocation = 1;
    record.summary.state = CardState::succeeded;
    record.record_io = true;
    record.inputs = {value};
    record.outputs = {value};
    original.write_sync([&](sql::Db& db) {
      History::insert_run(db, run, crypto::uuid4());
      History::put_artifact(db, {id,
                                 "1",
                                 {"card"},
                                 1,
                                 2,
                                 0,
                                 true,
                                 ".tsugumi/artifacts/1/card/@1/example",
                                 10,
                                 11,
                                 123,
                                 "tree:digest"});
      original.complete_invocation(db, record);
      History::put_last_input(db, {"card"}, "1", 1, {{"value", false, {value}}});
    });
  }
  History restored(database);
  restored.read([&](sql::Db& db) {
    for (const bool input : {false, true}) {
      const auto values = History::value_metas(db, "1", {"card"}, 1, input);
      ASSERT_EQ(values.size(), 1u);
      EXPECT_EQ(values[0].hash, crypto::sha256_hex(bytes));
      EXPECT_TRUE(values[0].artifacts.empty());
    }
    const auto saved = History::last_input(db, {"card"});
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->groups.at(0).values.at(0).schema, "schema-id");
    EXPECT_EQ(saved->groups.at(0).values.at(0).bytes, value.bytes);
    const auto artifact = History::artifact(db, id);
    ASSERT_TRUE(artifact);
    EXPECT_EQ(artifact->sealed_ms, 11);
    EXPECT_EQ(artifact->tree_hash, "tree:digest");
  });
}

TEST(RunHistory, RoundTripPreservesEveryRunFieldIncludingCommit) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("tsugumi-history-" + crypto::uuid4());
  std::filesystem::create_directories(directory);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  History original(directory / "original.sqlite");
  History restored(directory / "original.sqlite");
  for (const auto state :
       {RunState::succeeded, RunState::failed, RunState::cancelled}) {
    RunSummaryData input;
    input.status.run_id = std::to_string(static_cast<int>(state) + 1);
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
      input.status.failure = FailureData::make(
          FailureKind::cancelled, "diagnostic", {{"reason", "test"}});
      input.status.failure->current_rev = 19;
      input.status.failure->retry_after_ms = 50;
    }
    input.source_hash = "source fingerprint";
    input.target = {"scope", "card"};
    input.propagate = false;
    const auto op_id = crypto::uuid4();
    original.write_sync([&](sql::Db& db) {
      History::insert_run(db, input, op_id);
      History::put_artifact(
          db,
          {crypto::uuid4().substr(0, 16), *input.status.run_id, input.target, 1,
           1, 0, true, ".tsugumi/artifacts/test", 10, 11, 123, "tree:digest"});
    });
    const auto before = original.read(
        [&](sql::Db& db) { return History::run(db, *input.status.run_id); });
    const auto after = restored.read(
        [&](sql::Db& db) { return History::run(db, *input.status.run_id); });
    ASSERT_TRUE(before);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->status.run_id, before->status.run_id);
    EXPECT_EQ(after->status.state, before->status.state);
    EXPECT_TRUE(after->status.partial_run);
    EXPECT_TRUE(after->status.uses_partial_result);
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(wire::run_summary(builder, *after));
    const auto* exposed = flatbuffers::GetRoot<Tsugumi::RPC::RunSummary>(
        builder.GetBufferPointer());
    EXPECT_TRUE(exposed->status()->partial_run());
    EXPECT_TRUE(exposed->status()->uses_partial_result());
    EXPECT_EQ(after->status.rev, before->status.rev);
    EXPECT_EQ(after->status.execution_rev, before->status.execution_rev);
    EXPECT_EQ(after->status.accepted_at, before->status.accepted_at);
    EXPECT_EQ(after->status.started_at, before->status.started_at);
    EXPECT_EQ(after->status.ended_at, before->status.ended_at);
    EXPECT_EQ(after->status.commit, before->status.commit);
    ASSERT_EQ(after->status.failure.has_value(),
              before->status.failure.has_value());
    if (before->status.failure) {
      EXPECT_EQ(after->status.failure->kind, before->status.failure->kind);
      EXPECT_EQ(after->status.failure->args, before->status.failure->args);
      EXPECT_EQ(after->status.failure->diagnostic,
                before->status.failure->diagnostic);
      EXPECT_EQ(after->status.failure->current_rev,
                before->status.failure->current_rev);
      EXPECT_EQ(after->status.failure->retry_after_ms,
                before->status.failure->retry_after_ms);
    }
    EXPECT_EQ(after->source_hash, before->source_hash);
    EXPECT_EQ(after->target, before->target);
    EXPECT_EQ(after->propagate, before->propagate);
    restored.read([&](sql::Db& db) {
      auto st = db.prepare("SELECT op_id FROM runs WHERE run_id=?");
      st.bind(1, *input.status.run_id);
      ASSERT_TRUE(st.step());
      EXPECT_EQ(st.text(0), op_id);
    });
    // Both readers use kRunCols; the cursor sequence follows the two flags.
    for (auto* history : {&original, &restored}) {
      history->read([&](sql::Db& db) {
        const auto max = History::max_run_seq(db);
        std::vector<int64_t> seqs;
        const auto page =
            History::runs_page(db, input.target, true, max + 1, max, 1, &seqs);
        ASSERT_EQ(page.size(), 1u);
        EXPECT_EQ(seqs, std::vector<int64_t>{max});
        EXPECT_EQ(page[0].status.run_id, input.status.run_id);
        EXPECT_EQ(page[0].status.commit, input.status.commit);
        EXPECT_TRUE(page[0].status.partial_run);
        EXPECT_TRUE(page[0].status.uses_partial_result);
        const auto next =
            History::runs_page(db, input.target, true, seqs[0], max, 1, &seqs);
        if (max == 1)
          EXPECT_TRUE(next.empty());
        else {
          ASSERT_EQ(next.size(), 1u);
          EXPECT_NE(next[0].status.run_id, input.status.run_id);
          EXPECT_EQ(seqs.back(), max - 1);
        }
      });
    }
  }
}

TEST(RunHistory, SavedInputProvenanceSurvivesBothSnapshotPathsAndReplacement) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("tsugumi-history-" + crypto::uuid4());
  std::filesystem::create_directories(directory);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  History history(directory / "history.sqlite");
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

TEST(RunHistory, DeletionCannotExpandBeyondTheApprovedRuns) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("tsugumi-history-" + crypto::uuid4());
  std::filesystem::create_directories(directory);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  History history(directory / "history.sqlite");
  RunSummaryData approved, running;
  approved.status.run_id = "1";
  approved.status.state = RunState::succeeded;
  approved.status.ended_at = 10;
  running.status.run_id = "3";
  running.status.state = RunState::running;
  history.write_sync([&](sql::Db& db) {
    History::insert_run(db, approved, crypto::uuid4());
    History::insert_run(db, running, crypto::uuid4());
  });
  const std::vector<std::string> selected{*approved.status.run_id};
  // The second run was not shown for approval. It finishes before deletion.
  running.status.state = RunState::succeeded;
  running.status.ended_at = 20;
  history.write_sync(
      [&](sql::Db& db) { History::update_run(db, running.status); });

  EXPECT_TRUE(
      history.delete_runs(std::nullopt, std::vector<std::string>{}).empty());
  EXPECT_EQ(history.delete_runs(std::nullopt, selected), selected);
  history.read([&](sql::Db& db) {
    EXPECT_FALSE(History::run(db, *approved.status.run_id));
    EXPECT_TRUE(History::run(db, *running.status.run_id));
  });
  History restored(directory / "history.sqlite");
  restored.read([&](sql::Db& db) {
    EXPECT_FALSE(History::run(db, *approved.status.run_id));
    EXPECT_TRUE(History::run(db, *running.status.run_id));
  });
}

TEST(RunHistory, SelectedDeletionStillRequiresFinishedRunsBeforeTheCutoff) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("tsugumi-history-" + crypto::uuid4());
  std::filesystem::create_directories(directory);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  History history(directory / "history.sqlite");
  RunSummaryData old, recent, running;
  old.status.run_id = "1";
  old.status.state = RunState::succeeded;
  old.status.ended_at = 10;
  recent = old;
  recent.status.run_id = "2";
  recent.status.ended_at = 30;
  running.status.run_id = "3";
  running.status.state = RunState::running;
  history.write_sync([&](sql::Db& db) {
    for (const auto& run : {old, recent, running})
      History::insert_run(db, run, crypto::uuid4());
  });
  const std::vector<std::string> selected{
      *old.status.run_id, *recent.status.run_id, *running.status.run_id,
      crypto::uuid4(), *old.status.run_id};
  EXPECT_EQ(history.delete_runs(20, selected),
            std::vector<std::string>{*old.status.run_id});
  // The UI's all-runs request still deletes only finished runs.
  EXPECT_EQ(history.delete_runs(std::nullopt, std::nullopt),
            std::vector<std::string>{*recent.status.run_id});
  history.read([&](sql::Db& db) {
    EXPECT_TRUE(History::run(db, *running.status.run_id));
  });
}

}  // namespace
}  // namespace tsugumi
