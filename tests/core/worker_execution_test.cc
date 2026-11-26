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

TEST(WorkerOutputs, OpaqueBytesAreForwarded) {
  const std::vector<types::PortType> ports{{"value", false, nullptr}};
  const std::string bytes("not a FlatBuffer\0", 17);
  const auto outputs = accept_outputs({{"value", bytes}}, ports);
  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_EQ(outputs[0].name, "value");
  EXPECT_EQ(outputs[0].bytes, bytes);
}

TEST(WorkerOutputs, RejectsUnknownDuplicateAndMissingRequiredOutputs) {
  const std::vector<types::PortType> ports{{"value", true, nullptr}};
  try {
    accept_outputs({}, ports);
    FAIL() << "accepted a missing required output";
  } catch (const Failed& error) {
    EXPECT_EQ(error.failure.kind, FailureKind::output_invalid);
    ASSERT_EQ(error.failure.args.size(), 1u);
    EXPECT_EQ(error.failure.args.front(),
              std::make_pair(std::string("port"), std::string("value")));
  }
  EXPECT_THROW(accept_outputs({{"unknown", ""}}, ports), Failed);
  EXPECT_THROW(accept_outputs({{"value", ""}, {"value", ""}}, ports), Failed);
}

TEST(WorkerPoolOwnership, ClearsProjectScratch) {
  const auto root = stdfs::canonical(stdfs::temp_directory_path()) /
                    ("tsugumi-pool-" + crypto::uuid4());
  struct Cleanup {
    stdfs::path root;
    ~Cleanup() {
      std::error_code error;
      stdfs::remove_all(root, error);
    }
  } cleanup{root};
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
        worker_disposition(true, true, true, FailureData::make(kind));
    EXPECT_FALSE(result.keep);
  }
}

TEST(WorkerDisposition, UnknownProtocolStateAndDeadWorkersCannotBeReused) {
  EXPECT_FALSE(worker_disposition(true, true, false, {}).keep);
  EXPECT_FALSE(worker_disposition(true, false, true, {}).keep);
  EXPECT_FALSE(worker_disposition(false, true, true, {}).keep);
  EXPECT_TRUE(worker_disposition(true, true, true, {}).keep);
}

}  // namespace
}  // namespace tsugumi::workerd
