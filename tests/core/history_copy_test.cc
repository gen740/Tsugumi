#include "history.hh"

#include <gtest/gtest.h>

#include "util/crypto.hh"
#include "util/fs.hh"
#include "wire.hh"

namespace tsugumi {
namespace {

TEST(HistoryCopy, FileContentDigestsSurviveMissingFilesAndPortableHistory) {
  const auto directory = std::filesystem::temp_directory_path() / ("tsugumi-history-" + crypto::uuid4());
  std::filesystem::create_directories(directory);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  History original(directory / "original.sqlite", "secret");
  History restored(directory / "restored.sqlite", "secret");
  std::string error;
  const auto type = types::compile("table File { value:string; } root_type File;", "File", &error);
  ASSERT_TRUE(type) << error;
  const std::string bytes("\0\xff\x80\xc3(\n", 6);
  std::vector<std::string> runs, hashes;
  for (int i = 0; i < 3; ++i) {
    const auto file = directory / ("file-" + std::to_string(i));
    fs::write_atomic(file, i == 2 ? "other" : bytes);
    flatbuffers::FlatBufferBuilder builder;
    const auto text = builder.CreateString(file.string());
    const auto start = builder.StartTable();
    builder.AddOffset(4, text);
    builder.Finish(flatbuffers::Offset<flatbuffers::Table>(builder.EndTable(start)));
    Value value{"file", PortKind::File, *type,
                std::string(reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize()), {}};
    RunSummaryData run;
    run.status.run_id = crypto::uuid4();
    run.status.state = RunState::succeeded;
    InvocationRecordData invocation;
    invocation.summary.run_id = *run.status.run_id;
    invocation.summary.card = {"card"};
    invocation.summary.invocation = 1;
    invocation.summary.state = CardState::succeeded;
    invocation.record_io = true;
    invocation.inputs = {value};
    invocation.outputs = {value};
    original.write_sync([&](sql::Db& db) {
      History::insert_run(db, run, crypto::uuid4());
      original.complete_invocation(db, invocation);
    });
    hashes.push_back(crypto::sha256_hex(value.bytes));
    runs.push_back(*run.status.run_id);
    std::filesystem::remove(file);
  }
  ASSERT_NE(hashes[0], hashes[1]);
  ASSERT_EQ(original.export_runs(directory / "runs.sqlite"), 3u);
  ASSERT_EQ(restored.import_runs(directory / "runs.sqlite"), 3u);
  for (size_t i = 0; i < runs.size(); ++i) {
    for (const bool input : {false, true}) {
      restored.read([&](sql::Db& db) {
        const auto values = History::value_metas(db, runs[i], {"card"}, 1, input);
        ASSERT_EQ(values.size(), 1u);
        EXPECT_EQ(values[0].hash, hashes[i]);
        EXPECT_EQ(values[0].file_content_hash, crypto::sha256_hex(i == 2 ? "other" : bytes));
      });
    }
  }
}

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

TEST(HistoryCopy, DeletionCannotExpandBeyondTheApprovedRuns) {
  const auto directory = std::filesystem::temp_directory_path() / ("tsugumi-history-" + crypto::uuid4());
  std::filesystem::create_directories(directory);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  History history(directory / "history.sqlite", "secret");
  const auto copy = directory / "copy.sqlite";
  RunSummaryData approved, running;
  approved.status.run_id = crypto::uuid4();
  approved.status.state = RunState::succeeded;
  approved.status.ended_at = 10;
  running.status.run_id = crypto::uuid4();
  running.status.state = RunState::running;
  history.write_sync([&](sql::Db& db) {
    History::insert_run(db, approved, crypto::uuid4());
    History::insert_run(db, running, crypto::uuid4());
  });
  ASSERT_EQ(history.export_runs(copy), 1);
  const std::vector<std::string> selected{*approved.status.run_id};
  // The second run was not shown for approval. It finishes before deletion.
  running.status.state = RunState::succeeded;
  running.status.ended_at = 20;
  history.write_sync([&](sql::Db& db) { History::update_run(db, running.status); });
  ASSERT_EQ(history.export_runs(copy), 1);

  EXPECT_TRUE(history.delete_runs(copy, std::nullopt, std::vector<std::string>{}).empty());
  EXPECT_EQ(history.delete_runs(copy, std::nullopt, selected), selected);
  history.read([&](sql::Db& db) {
    EXPECT_FALSE(History::run(db, *approved.status.run_id));
    EXPECT_TRUE(History::run(db, *running.status.run_id));
  });
  History restored(directory / "restored.sqlite", "secret");
  ASSERT_EQ(restored.import_runs(copy), 1);
  restored.read([&](sql::Db& db) {
    EXPECT_FALSE(History::run(db, *approved.status.run_id));
    EXPECT_TRUE(History::run(db, *running.status.run_id));
  });
}

TEST(HistoryCopy, SelectedDeletionStillRequiresFinishedRunsBeforeTheCutoff) {
  const auto directory = std::filesystem::temp_directory_path() / ("tsugumi-history-" + crypto::uuid4());
  std::filesystem::create_directories(directory);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  History history(directory / "history.sqlite", "secret");
  RunSummaryData old, recent, running;
  old.status.run_id = crypto::uuid4();
  old.status.state = RunState::succeeded;
  old.status.ended_at = 10;
  recent = old;
  recent.status.run_id = crypto::uuid4();
  recent.status.ended_at = 30;
  running.status.run_id = crypto::uuid4();
  running.status.state = RunState::running;
  history.write_sync([&](sql::Db& db) {
    for (const auto& run : {old, recent, running}) History::insert_run(db, run, crypto::uuid4());
  });
  const auto copy = directory / "absent.sqlite";
  const std::vector<std::string> selected{
      *old.status.run_id, *recent.status.run_id, *running.status.run_id, crypto::uuid4(), *old.status.run_id};
  EXPECT_EQ(history.delete_runs(copy, 20, selected), std::vector<std::string>{*old.status.run_id});
  // The UI's all-runs request still deletes only finished runs.
  EXPECT_EQ(history.delete_runs(copy, std::nullopt, std::nullopt), std::vector<std::string>{*recent.status.run_id});
  history.read([&](sql::Db& db) { EXPECT_TRUE(History::run(db, *running.status.run_id)); });
}

}  // namespace
}  // namespace tsugumi
