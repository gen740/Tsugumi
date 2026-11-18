#include "tsugumi/workerd/worker_execution.hh"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi::workerd {
namespace {
namespace stdfs = std::filesystem;

class WorkerDelivery : public testing::TestWithParam<OutputDelivery> {
 protected:
  auto SetUp() -> void override {
    root_ = stdfs::canonical(stdfs::temp_directory_path()) /
            ("tsugumi-delivery-" + crypto::uuid4());
    stdfs::create_directories(root_ / "worker/out");
    stdfs::create_directories(root_ / "worker/inputs");
    std::string error;
    const auto compiled =
        types::compile("table Path { value:string; }", "Path", &error);
    ASSERT_TRUE(compiled) << error;
    type_ = *compiled;
    output_ = ::open((root_ / "worker/out").c_str(),
                     O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    input_ = ::open((root_ / "worker/inputs").c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ASSERT_GE(output_, 0);
    ASSERT_GE(input_, 0);
  }
  auto TearDown() -> void override {
    if (output_ >= 0) ::close(output_);
    if (input_ >= 0) ::close(input_);
    std::error_code error;
    stdfs::remove_all(root_, error);
  }
  auto value(const std::string& name, PortKind kind, const stdfs::path& path)
      -> Value {
    std::string bytes, error;
    EXPECT_TRUE(types::json_to_flatbuffer(*type_, R"({"value":"placeholder"})",
                                          &bytes, &error))
        << error;
    return with_path({name, kind, type_, bytes, {}}, path.string());
  }
  auto stage(WorkerOutcome& outcome, PortKind kind,
             const std::vector<Value>& inputs = {},
             const std::vector<Value>& original = {}) -> void {
    const std::atomic<bool> cancel{false};
    if (outcome.failure) return;
    const std::vector<types::PortType> ports{{"result", kind, false, type_}};
    auto outputs = accept_outputs(outcome.outputs, ports);
    stage_outputs(outputs, inputs, original, output_,
                  (root_ / "worker/out").string(), input_,
                  root_ / "worker/inputs", root_ / "result/files/out",
                  GetParam(), cancel);
    for (size_t i = 0; i < outputs.size(); ++i)
      outcome.outputs[i].second = std::move(outputs[i].bytes);
  }
  stdfs::path root_;
  types::TypeRef type_;
  int output_ = -1;
  int input_ = -1;
};

TEST_P(WorkerDelivery, CopiesOnlyDeclaredFiles) {
  fs::write_atomic(root_ / "worker/out/declared", "declared");
  fs::write_atomic(root_ / "worker/out/unused", "unused");
  const auto produced =
      value("result", PortKind::File, root_ / "worker/out/declared");
  WorkerOutcome outcome{{{"result", produced.bytes}}, {}};
  stage(outcome, PortKind::File);
  EXPECT_EQ(fs::read_file(root_ / "result/files/out/declared"), "declared");
  EXPECT_FALSE(stdfs::exists(root_ / "result/files/out/unused"));
  const auto result = path_value(
      Value{"result", PortKind::File, type_, outcome.outputs[0].second, {}});
  EXPECT_EQ(result, GetParam() == OutputDelivery::local_paths
                        ? (root_ / "worker/out/declared").string()
                        : "out/declared");
}

TEST_P(WorkerDelivery, CopiesDeclaredDirectoryAndMaterializesInternalLinks) {
  fs::write_atomic(root_ / "worker/out/tree/nested/file", "directory");
  stdfs::create_directories(root_ / "worker/out/tree/empty");
  stdfs::create_symlink("nested/file", root_ / "worker/out/tree/link");
  const auto produced =
      value("result", PortKind::Directory, root_ / "worker/out/tree");
  WorkerOutcome outcome{{{"result", produced.bytes}}, {}};
  stage(outcome, PortKind::Directory);
  EXPECT_EQ(fs::read_file(root_ / "result/files/out/tree/link"), "directory");
  EXPECT_FALSE(stdfs::is_symlink(root_ / "result/files/out/tree/link"));
  EXPECT_TRUE(stdfs::is_directory(root_ / "result/files/out/tree/empty"));
}

TEST_P(WorkerDelivery, TransferFailureDoesNotBecomeACardFailure) {
  const auto produced =
      value("result", PortKind::File, root_ / "worker/out/missing");
  WorkerOutcome outcome{{{"result", produced.bytes}}, {}};
  EXPECT_THROW(stage(outcome, PortKind::File), Failed);
  EXPECT_FALSE(outcome.failure);
  EXPECT_TRUE(
      worker_disposition(true, true, true, false, outcome.failure).keep);
}

TEST_P(WorkerDelivery, EscapingLinkIsATransferFailureAndCannotReadOutside) {
  fs::write_atomic(root_ / "outside", "private");
  stdfs::create_symlink(root_ / "outside", root_ / "worker/out/link");
  const auto produced =
      value("result", PortKind::File, root_ / "worker/out/link");
  WorkerOutcome outcome{{{"result", produced.bytes}}, {}};
  EXPECT_THROW(stage(outcome, PortKind::File), Failed);
  EXPECT_FALSE(outcome.failure);
  EXPECT_FALSE(stdfs::exists(root_ / "result/files/out/link"));
}

TEST_P(WorkerDelivery, ForwardedInputsKeepCarrierSpecificDelivery) {
  fs::write_atomic(root_ / "worker/inputs/file", "forwarded");
  const auto staged =
      value("input", PortKind::File, root_ / "worker/inputs/file");
  const auto original =
      value("input", PortKind::File, root_ / "recorded/original");
  WorkerOutcome outcome{{{"result", staged.bytes}}, {}};
  stage(outcome, PortKind::File, {staged}, {original});
  const auto delivered = path_value(
      Value{"result", PortKind::File, type_, outcome.outputs[0].second, {}});
  ASSERT_TRUE(delivered);
  if (GetParam() == OutputDelivery::local_paths) {
    EXPECT_EQ(*delivered, (root_ / "recorded/original").string());
    EXPECT_FALSE(stdfs::exists(root_ / "result/files"));
  } else {
    EXPECT_TRUE(delivered->starts_with("out/.forwarded-"));
    EXPECT_EQ(fs::read_file(root_ / "result/files" / *delivered), "forwarded");
  }
}

TEST_P(WorkerDelivery, FailedCardDoesNotTransferOutputs) {
  const auto produced =
      value("result", PortKind::File, root_ / "worker/out/missing");
  WorkerOutcome outcome{{{"result", produced.bytes}},
                        FailureData::make(FailureKind::worker_failed)};
  EXPECT_NO_THROW(stage(outcome, PortKind::File));
  EXPECT_FALSE(stdfs::exists(root_ / "result/files"));
  EXPECT_FALSE(
      worker_disposition(true, true, true, false, outcome.failure).keep);
}

TEST_P(WorkerDelivery, InvalidOutputIsRejectedBeforeTransfer) {
  WorkerOutcome outcome{{{"unknown", "invalid buffer"}}, {}};
  EXPECT_THROW(stage(outcome, PortKind::File), Failed);
}

INSTANTIATE_TEST_SUITE_P(Carriers, WorkerDelivery,
                         testing::Values(OutputDelivery::local_paths,
                                         OutputDelivery::transferred_files));

// A supervised job already owns its scratch tree. Merely constructing the
// one-shot pool must not remove it, including when acquire cannot construct a
// worker. Ordinary pools retain their existing project-lifetime cleanup.
TEST(WorkerPoolOwnership, OneShotLeavesJobScratchToTheWorkerAndSupervisor) {
  const auto root = stdfs::canonical(stdfs::temp_directory_path()) /
                    ("tsugumi-pool-" + crypto::uuid4());
  struct Cleanup {
    stdfs::path root;
    ~Cleanup() {
      std::error_code error;
      stdfs::remove_all(root, error);
    }
  } cleanup{root};
  const auto job = root / "job/scratch";
  fs::write_atomic(job / "work/partial", "job scratch");
  {
    WorkerPool pool(job, {}, WorkerPoolMode::one_shot);
    EXPECT_EQ(fs::read_file(job / "work/partial"), "job scratch");
  }
  EXPECT_EQ(fs::read_file(job / "work/partial"), "job scratch");
  const auto project = root / "project/workers";
  fs::write_atomic(project / "stale", "old worker scratch");
  {
    WorkerPool pool(project);
    EXPECT_FALSE(stdfs::exists(project / "stale"));
  }
  EXPECT_FALSE(stdfs::exists(project));
}

TEST(WorkerDisposition, CardFailuresAndMissingRequiredOutputsDiscardResidents) {
  for (const auto kind :
       {FailureKind::worker_failed, FailureKind::output_invalid,
        FailureKind::cancelled, FailureKind::timeout}) {
    const auto result =
        worker_disposition(true, true, true, false, FailureData::make(kind));
    EXPECT_FALSE(result.keep);
    EXPECT_FALSE(result.reason.empty());
  }
  EXPECT_FALSE(worker_disposition(true, true, true, true, {}).keep);
}

TEST(WorkerDisposition, UnknownProtocolStateAndDeadWorkersCannotBeReused) {
  EXPECT_FALSE(worker_disposition(true, true, false, false, {}).keep);
  EXPECT_FALSE(worker_disposition(true, false, true, false, {}).keep);
  EXPECT_FALSE(worker_disposition(false, true, true, false, {}).keep);
  EXPECT_TRUE(worker_disposition(true, true, true, false, {}).keep);
}

}  // namespace
}  // namespace tsugumi::workerd
