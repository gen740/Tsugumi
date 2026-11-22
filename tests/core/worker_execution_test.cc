#include "tsugumi/workerd/worker_execution.hh"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>

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
    stdfs::create_directories(root_ / "worker/work");
    stdfs::create_directories(root_ / "worker/inputs");
    output_ = ::open((root_ / "worker/work").c_str(),
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
  static auto input(const std::string& name, const stdfs::path& path)
      -> Value {
    Value value{name, PortKind::Path, nullptr, {}, {}};
    value.path = path.string();
    return value;
  }
  // Stages one Path output and returns its delivered path.
  auto stage(RawOutput output, const std::vector<Value>& inputs = {},
             const std::vector<Value>& original = {},
             fs::OutputBudget budget = {}) -> std::string {
    const std::atomic<bool> cancel{false};
    const std::vector<types::PortType> ports{
        {"result", PortKind::Path, false, nullptr}};
    std::vector<std::pair<std::string, std::string>> forwards;
    if (!output.forwarded_from.empty())
      forwards.emplace_back(output.name, output.forwarded_from);
    auto outputs = accept_outputs({std::move(output)}, ports);
    stage_outputs(outputs, inputs, original, forwards, output_,
                  (root_ / "worker/work").string(), root_ / "worker/inputs",
                  root_ / "result/files/out", GetParam(), cancel, {}, budget);
    return outputs.at(0).path;
  }
  static auto produced(const stdfs::path& path) -> RawOutput {
    return {"result", {}, path.string(), {}};
  }
  stdfs::path root_;
  int output_ = -1;
  int input_ = -1;
};

TEST_P(WorkerDelivery, CopiesOnlyTheReturnedFile) {
  fs::write_atomic(root_ / "worker/work/declared", "declared");
  fs::write_atomic(root_ / "worker/work/unused", "unused");
  const auto result = stage(produced(root_ / "worker/work/declared"));
  EXPECT_EQ(fs::read_file(root_ / "result/files/out/declared"), "declared");
  EXPECT_FALSE(stdfs::exists(root_ / "result/files/out/unused"));
  EXPECT_EQ(result, GetParam() == OutputDelivery::local_paths
                        ? (root_ / "result/files/out/declared").string()
                        : "out/declared");
}

TEST_P(WorkerDelivery, RelativePathIsUnderTheWorkingDirectory) {
  fs::write_atomic(root_ / "worker/work/relative", "relative");
  stage(produced("relative"));
  EXPECT_EQ(fs::read_file(root_ / "result/files/out/relative"), "relative");
}

TEST_P(WorkerDelivery, CopiesTheWorkingDirectoryWhenReturned) {
  fs::write_atomic(root_ / "worker/work/result.txt", "directory");
  const auto result = stage(produced("."));
  EXPECT_EQ(fs::read_file(root_ / "result/files/out/result.txt"), "directory");
  EXPECT_EQ(result, GetParam() == OutputDelivery::local_paths
                        ? (root_ / "result/files/out").string()
                        : "out");
}

TEST_P(WorkerDelivery, CopiesOnlyRegularDirectoryContents) {
  fs::write_atomic(root_ / "worker/work/tree/nested/file", "directory");
  stdfs::create_directories(root_ / "worker/work/tree/empty");
  stage(produced(root_ / "worker/work/tree"));
  EXPECT_EQ(fs::read_file(root_ / "result/files/out/tree/nested/file"),
            "directory");
  EXPECT_TRUE(stdfs::is_directory(root_ / "result/files/out/tree/empty"));
}

TEST_P(WorkerDelivery, PathOutsideTheWorkingDirectoryIsRejected) {
  fs::write_atomic(root_ / "outside", "private");
  EXPECT_THROW(stage(produced(root_ / "outside")), Failed);
  EXPECT_THROW(stage(produced("../outside")), Failed);
  EXPECT_FALSE(stdfs::exists(root_ / "result/files/out"));
}

TEST_P(WorkerDelivery, EscapingLinkIsATransferFailureAndCannotReadOutside) {
  fs::write_atomic(root_ / "outside", "private");
  stdfs::create_symlink(root_ / "outside", root_ / "worker/work/link");
  EXPECT_THROW(stage(produced(root_ / "worker/work/link")), Failed);
  EXPECT_FALSE(stdfs::exists(root_ / "result/files/out/link"));
}

TEST_P(WorkerDelivery, SpecialFileIsNeitherAFileNorADirectory) {
  ASSERT_EQ(::mkfifo((root_ / "worker/work/fifo").c_str(), 0600), 0);
  EXPECT_THROW(stage(produced(root_ / "worker/work/fifo")), Failed);
}

TEST_P(WorkerDelivery, ForwardedInputsKeepCarrierSpecificDelivery) {
  fs::write_atomic(root_ / "worker/inputs/inputs/0/file", "forwarded");
  const auto staged = input("input", root_ / "worker/inputs/inputs/0/file");
  const auto original = input("input", root_ / "recorded/original");
  auto output = produced(staged.path);
  output.forwarded_from = "input";
  const auto delivered = stage(output, {staged}, {original}, {0, 0});
  if (GetParam() == OutputDelivery::local_paths) {
    EXPECT_EQ(delivered, (root_ / "recorded/original").string());
  } else {
    EXPECT_EQ(delivered, "inputs/0/file");
  }
  EXPECT_FALSE(stdfs::exists(root_ / "result/files"));
}

TEST_P(WorkerDelivery, ForwardMustBeTheInputPathExactly) {
  fs::write_atomic(root_ / "worker/inputs/inputs/0/file", "forwarded");
  const auto staged = input("input", root_ / "worker/inputs/inputs/0/file");
  auto output = produced(root_ / "worker/inputs/inputs/0");
  output.forwarded_from = "input";
  EXPECT_THROW(stage(output, {staged}, {staged}), Failed);
}

TEST_P(WorkerDelivery, DefaultBudgetRejectsSparseFileOver64GiB) {
  const auto file = root_ / "worker/work/large";
  fs::write_atomic(file, "");
  stdfs::resize_file(file, (64ull << 30) + 1);
  try {
    stage(produced(file));
    FAIL() << "accepted an oversized sparse file";
  } catch (const Failed& error) {
    EXPECT_EQ(error.failure.kind, FailureKind::output_invalid);
    EXPECT_NE(std::string_view(error.what()).find("byte budget"),
              std::string_view::npos);
  }
}

TEST_P(WorkerDelivery, DefaultBudgetRejectsMoreThan1000Entries) {
  const auto dir = root_ / "worker/work/many";
  stdfs::create_directory(dir);
  for (unsigned i = 0; i < 1000; ++i) std::ofstream(dir / std::to_string(i));
  try {
    stage(produced(dir));
    FAIL() << "accepted an oversized directory";
  } catch (const Failed& error) {
    EXPECT_EQ(error.failure.kind, FailureKind::output_invalid);
    EXPECT_NE(std::string_view(error.what()).find("entry budget"),
              std::string_view::npos);
  }
}

TEST_P(WorkerDelivery, UnknownOutputIsRejectedBeforeTransfer) {
  EXPECT_THROW(stage({"unknown", "invalid buffer", {}, {}}), Failed);
}

TEST(WorkerOutputs, FlatBufferBytesAreAcceptedUnopened) {
  const std::vector<types::PortType> ports{
      {"value", PortKind::FlatBuffer, false, nullptr}};
  const auto outputs =
      accept_outputs({{"value", std::string("not a FlatBuffer\0", 17), {}, {}}},
                     ports);
  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_EQ(outputs[0].bytes, std::string("not a FlatBuffer\0", 17));
}

TEST(WorkerOutputs, PathAndFlatBufferUseSeparateFields) {
  const std::vector<types::PortType> path{
      {"value", PortKind::Path, false, nullptr}};
  const std::vector<types::PortType> blob{
      {"value", PortKind::FlatBuffer, false, nullptr}};
  EXPECT_THROW(accept_outputs({{"value", "bytes", "path", {}}}, path),
               Failed);
  EXPECT_THROW(accept_outputs({{"value", {}, "path", {}}}, blob), Failed);
  EXPECT_THROW(accept_outputs({{"value", "bytes", {}, "input"}}, blob),
               Failed);
}

TEST(WorkerOutputs, MissingRequiredOutputIsRejected) {
  const std::vector<types::PortType> ports{
      {"value", PortKind::FlatBuffer, false, nullptr}};
  EXPECT_THROW(accept_outputs({}, ports), Failed);
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
