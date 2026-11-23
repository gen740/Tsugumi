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

TEST(WorkerOutputs, OpaqueBytesAndGrantedArtifactsAreForwarded) {
  const ArtifactKey id(16, 'a');
  const std::vector<Value> inputs{{"input", nullptr, {}, "schema", {id}}};
  const std::vector<types::PortType> ports{{"value", false, nullptr}};
  const std::string bytes("not a FlatBuffer\0", 17);
  const auto outputs = accept_outputs({{"value", bytes, {id}}}, ports, inputs);
  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_EQ(outputs[0].bytes, bytes);
  EXPECT_EQ(outputs[0].artifacts, std::vector<ArtifactKey>{id});
}

TEST(WorkerOutputs, OnlyCreatedAndGrantedArtifactsAreAccepted) {
  const ArtifactKey id(16, 'a');
  const std::vector<ArtifactKey> created{id};
  const std::vector<types::PortType> ports{{"value", false, nullptr}};
  EXPECT_THROW(accept_outputs({{"value", "", {id}}}, ports), Failed);
  EXPECT_NO_THROW(accept_outputs({{"value", "", {id}}}, ports, {}, created));
  const std::vector<ArtifactKey> duplicates{id, id};
  EXPECT_THROW(accept_outputs({{"value", "", {id}}}, ports, {}, duplicates), Failed);
}

TEST(WorkerOutputs, RejectsUnknownDuplicateAndMissingRequiredOutputs) {
  const std::vector<types::PortType> ports{{"value", true, nullptr}};
  EXPECT_THROW(accept_outputs({}, ports), Failed);
  EXPECT_THROW(accept_outputs({{"unknown", "", {}}}, ports), Failed);
  EXPECT_THROW(accept_outputs({{"value", "", {}}, {"value", "", {}}}, ports), Failed);
}

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
