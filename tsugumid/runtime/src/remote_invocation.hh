#pragma once

#include "execution_service.hh"
#include "remote_job.hh"
#include "tsugumi/workerd/invocation.hh"

namespace tsugumi {

struct RemoteInvocationResult {
  std::vector<Value> outputs;
  std::optional<FailureData> failure;
  BuildEnvironmentData environment;
  // Entries after the recorded prefix.
  std::vector<std::pair<LogStream, std::string>> logs;
  bool built = false;
  bool log_truncated = false;
};

// The prefix of a job's JSONL log that the core already recorded while the
// job ran: its length and SHA-256. Import verifies and skips it.
struct RemoteLogPrefix {
  uint64_t bytes = 0;
  std::string sha256;
};

// Decodes one JSONL log entry (without its newline). Throws when invalid.
auto decode_remote_log_entry(std::string_view line)
    -> std::pair<LogStream, std::string>;

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
    const std::filesystem::path& module_cache,
    const std::vector<types::PortType>& outputs,
    const WorkerProc::DisplaySink& display, const RemoteLogPrefix& recorded,
    std::stop_token stop,
    const std::function<void(std::string_view, uint64_t, uint64_t)>& progress,
    const std::function<void(int64_t, int64_t)>& on_validation)
    -> RemoteInvocationResult;

}  // namespace tsugumi
