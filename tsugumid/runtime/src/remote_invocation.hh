#pragma once

#include "execution_service.hh"
#include "remote_job.hh"
#include "tsugumi/workerd/invocation.hh"

namespace tsugumi {

struct RemoteInvocationResult {
  std::vector<Value> outputs;
  std::optional<FailureData> failure;
  // Completed remote build and journal entries commit with the invocation.
  std::vector<std::pair<LogStream, std::string>> auxiliary_logs = {};
  bool auxiliary_logs_truncated = false;
  BuildEnvironmentData environment;
  bool built = false;
};

// The prefix of a job's log (its size-prefixed entries) that the core already
// recorded while the job ran: its length and SHA-256. Import verifies and
// skips it.
struct RemoteLogPrefix {
  uint64_t bytes = 0;
  std::string sha256;
};

auto stage_remote_archive(const RemoteJobClient& client,
                          const std::string& service,
                          const std::string& instance,
                          const std::string& job_id,
                          const std::filesystem::path& archive,
                          std::stop_token cancel,
                          const TransferProgress& progress = {}) -> std::string;

auto stage_remote_invocation(
    const RemoteJobClient& client, const std::string& service,
    const std::string& instance, const std::string& job_id,
    const std::vector<std::filesystem::path>& file_roots,
    const std::string& archive_digest, const ExecutionRequest& request)
    -> std::string;
auto import_remote_result(
    const RemoteJobClient& client, const std::string& service,
    const std::string& instance, const std::string& job_id,
    const std::string& result_digest, const std::string& spec,
    const std::filesystem::path& directory,
    const std::vector<types::PortType>& outputs,
    const std::vector<Value>& inputs,
    const RemoteLogPrefix& recorded,
    const WorkerProc::LogSink& log,
    std::stop_token stop,
    const std::function<void(std::string_view, uint64_t, uint64_t)>& progress,
    const std::function<void(int64_t, int64_t)>& on_validation)
    -> RemoteInvocationResult;

}  // namespace tsugumi
