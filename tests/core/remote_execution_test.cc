#include <gtest/gtest.h>

#include "scheduler.hh"
#include "service.hh"
#include "test_document.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi {
namespace {
TEST(RemoteExecutionCapabilities, FiltersUnsupportedLimits) {
  Node node;
  node.path = {"work"};
  node.settings.sandbox.limits = ExecutionResources{500, 256u << 20};
  RemoteTarget target;
  target.id = "remote";
  target.available = true;
  EXPECT_TRUE(Service::execution_diagnostic(node, {target}));
  target.resource_limits = true;
  EXPECT_FALSE(Service::execution_diagnostic(node, {target}));
}
TEST(RemoteExecutionCapabilities,
     ReportsUnavailableMatchingWorkerWithHostReason) {
  Node node;
  node.path = {"work"};
  RemoteTarget local;
  local.id = "local";
  local.available = false;
  local.namespace_isolation = true;
  RemoteTarget remote;
  remote.id = "remote";
  remote.unavailable_reason = "host refusal for remote";
  auto diagnostic = Service::execution_diagnostic(node, {local, remote});
  ASSERT_TRUE(diagnostic);
  EXPECT_TRUE(diagnostic->error);
  EXPECT_EQ(diagnostic->card, node.path);
  EXPECT_EQ(diagnostic->message, remote.unavailable_reason);
  remote.available = true;
  EXPECT_FALSE(Service::execution_diagnostic(node, {local, remote}));
  remote.available = false;
  local.available = true;
  const auto local_diagnostic =
      Service::execution_diagnostic(node, {local, remote});
  EXPECT_FALSE(local_diagnostic && local_diagnostic->error);
}

TEST(RemoteExecutionLifetime,
     StartupInterruptsLocalAndRemoteWithoutAWorkerOrResults) {
  namespace fs = std::filesystem;
  const auto root = fs::canonical(fs::temp_directory_path()) /
                    ("core-lifetime-" + crypto::uuid4());
  struct Cleanup {
    fs::path root;
    ~Cleanup() { fs::remove_all(root); }
  } cleanup{root};
  fs::create_directories(root);
  tsugumi::fs::write_atomic(
      root / "tsugumi.json",
      test::project_json(
          R"({"nixPath": [],"canvas":{"cards":[],"connections":[]}})"));
  ServiceOptions options;
  options.root = root;
  options.scratch_dir = root / "scratch";
  options.run_scratch_dir = root / "outputs";
  const auto local_run = crypto::uuid4(), remote_run = crypto::uuid4(),
             stopped_run = crypto::uuid4();
  const std::string fingerprint = crypto::sha256_hex("spec");
  {
    Service previous(options);
    previous.open();
    previous.history().write_sync([&](sql::Db& db) {
      for (const auto& id : {local_run, remote_run, stopped_run}) {
        RunSummaryData run;
        run.status.run_id = id;
        run.status.state =
            id == stopped_run ? RunState::failed : RunState::running;
        History::insert_run(db, run, crypto::uuid4());
        InvocationSummaryData invocation;
        invocation.run_id = id;
        invocation.card = {"work"};
        invocation.invocation = 1;
        invocation.state = CardState::running;
        invocation.started_at = now_ms() - 1000;
        invocation.stages.push_back({Phase::execute, StageOutcome::running,
                                     *invocation.started_at, std::nullopt});
        if (id != local_run) {
          invocation.execution_service = "unreachable-worker";
          invocation.execution_job_id = crypto::sha256_hex(id);
          invocation.execution_state = RemoteExecutionState::running;
          History::put_remote_job(db,
                                  {invocation, std::string(64, 'a'), "spec"});
        }
        History::put_status(db, invocation);
      }
    });
  }
  Service current(options);
  current
      .open();  // No RPC server, broker or worker is needed to settle history.
  current.history().read([&](sql::Db& db) {
    for (const auto& id : {local_run, remote_run, stopped_run}) {
      const auto run = History::run(db, id);
      ASSERT_TRUE(run);
      EXPECT_EQ(run->status.state, RunState::failed);
      ASSERT_TRUE(run->status.failure);
      EXPECT_EQ(run->status.failure->kind, FailureKind::interrupted);
      EXPECT_EQ(run->status.failure->diagnostic,
                "tsugumid restarted while the run was active");
      const auto record = History::invocation(db, id, {"work"}, 1);
      ASSERT_TRUE(record);
      EXPECT_EQ(record->summary.state, CardState::failed);
      ASSERT_TRUE(record->summary.started_at);
      ASSERT_TRUE(record->summary.ended_at);
      EXPECT_EQ(record->summary.ended_at, run->status.ended_at);
      EXPECT_EQ(record->summary.duration_ms,
                *record->summary.ended_at - *record->summary.started_at);
      ASSERT_EQ(record->summary.stages.size(), 1);
      EXPECT_EQ(record->summary.stages.back().outcome, StageOutcome::failed);
      EXPECT_TRUE(record->summary.stages.back().end);
      ASSERT_TRUE(record->summary.failure);
      EXPECT_EQ(record->summary.failure->to_bytes(),
                run->status.failure->to_bytes());
      EXPECT_TRUE(record->outputs.empty());
      if (id != local_run)
        EXPECT_EQ(record->summary.execution_state,
                  RemoteExecutionState::interrupted);
      else
        EXPECT_FALSE(record->summary.execution_state);
    }
    EXPECT_FALSE(History::has_remote_jobs(db));
    auto releases =
        db.prepare("SELECT fingerprint,delete_data FROM remote_releases");
    int count = 0;
    while (releases.step()) {
      EXPECT_EQ(releases.text(0), fingerprint);
      EXPECT_EQ(releases.i64(1), 0);
      ++count;
    }
    EXPECT_EQ(count, 2);
    EXPECT_TRUE(History::unfinished_runs(db).empty());
  });
  // A further restart cannot turn interruption into success or schedule work.
  Scheduler scheduler(current);
  scheduler.recover_interrupted();
  EXPECT_FALSE(scheduler.active_status());
}
}  // namespace
}  // namespace tsugumi
