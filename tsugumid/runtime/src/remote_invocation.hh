#pragma once

#include "execution_service.hh"
#include "remote_job.hh"
#include "tsugumi/workerd/invocation.hh"

namespace tsugumi {

struct RemoteInvocationResult {
  WorkerOutcome worker;
  BuildEnvironmentData environment;
  std::vector<std::pair<LogStream, std::string>> logs;
  bool built = false;
  bool log_truncated = false;
};

auto stage_remote_archive(const RemoteJobClient& client,
                          const std::string& service,
                          const std::string& instance,
                          const std::string& job_id,
                          const std::filesystem::path& archive,
                          const std::atomic<bool>& cancel,
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
    const std::filesystem::path& module_cache,
    const std::vector<types::PortType>& outputs,
    const WorkerProc::DisplaySink& display, const std::atomic<bool>& stop,
    const std::function<void(std::string_view, uint64_t, uint64_t)>& progress =
        {}) -> RemoteInvocationResult;

}  // namespace tsugumi
