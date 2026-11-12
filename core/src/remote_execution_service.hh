// Runs Card invocations on remote execution daemons as durable jobs: the
// derivation, inputs and call are uploaded, the job is registered in the
// history before it is submitted, and its result is imported when it ends.
// A job outlives the process that started it; recovery completes it later.
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "execution_service.hh"
#include "history.hh"
#include "remote_job.hh"

namespace tsugumi {

// The completion of a job a previous process left, imported from its service.
struct RecoveredRemoteJob {
  // Inputs, outputs, input sources and display of the completion; its
  // summary is the job's invocation.
  InvocationRecordData record;
  std::vector<std::pair<LogStream, std::string>> logs;
  std::optional<ArtifactData> artifact;
  BuildEnvironmentData environment;
};

class RemoteExecutionService final : public ExecutionService {
 public:
  RemoteExecutionService(Service& service, RemoteJobClient& client);

  // Every invocation exports its own derivation for the service to build.
  auto reuses_builds() const -> bool override { return false; }
  auto prepare(const Node& node, const std::filesystem::path& directory, BuildRequest& build)
      -> std::unique_ptr<PreparedExecution> override;

  // One exchange about a job a previous process left; it is never submitted
  // again. Updates the job's execution state and acceptance. Empty when the
  // service could not be reached.
  auto poll_recovered(RemoteJobRecord& job) -> std::optional<RemoteJobState>;
  // The completion of a recovered job that ended with `reply`. Sets the
  // invocation's environment, failure and execution state. Throws
  // BrokerDisconnected when the result must be fetched again.
  auto recover(RemoteJobRecord& job, const RemoteJobState& reply, const std::atomic<bool>& stop)
      -> RecoveredRemoteJob;

 private:
  Service& service_;
  RemoteJobClient& client_;
};

// Copies an output of a remote job into the run's artifacts, under an id
// derived from the job's id (the same after a recovery).
auto prepare_remote_artifact(const std::filesystem::path& run_dir, const std::filesystem::path& out_dir,
                             const InvocationSummaryData& inv, Value& value) -> ArtifactData;

}  // namespace tsugumi
