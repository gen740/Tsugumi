#pragma once

#include "execution_service.hh"
#include "remote_job.hh"
#include "tsugumi/workerd/invocation.hh"

namespace tsugumi {



struct RemoteInvocationResult {
  WorkerOutcome worker;
  BuildEnvironmentData environment;
  bool built = false;
};

struct RemoteRecoveryContract {
  std::vector<types::PortType> outputs;
  std::vector<Value> inputs;
  std::vector<InputSourceData> sources;
  bool record_io;
  std::string artifact_output;
};

auto remote_recovery_contract(const Node& node, const std::vector<Value>& inputs,
                              const std::vector<InputSourceData>& sources) -> std::string;
auto remote_recovery_contract(const std::string& encoded) -> RemoteRecoveryContract;

auto stage_remote_archive(const RemoteJobClient& client, const std::string& service, const std::string& server_id,
                          const std::string& instance, const std::string& job_id, const std::filesystem::path& archive,
                          const std::atomic<bool>& cancel) -> std::string;

auto stage_remote_invocation(const RemoteJobClient& client, const std::string& service, const std::string& server_id,
                             const std::string& instance, const std::string& job_id, const std::string& project_id,
                             const std::vector<std::filesystem::path>& file_roots, const std::string& archive_digest,
                             const ExecutionRequest& request) -> std::string;
auto import_remote_result(const RemoteJobClient& client, const std::string& service, const std::string& server_id,
                          const std::string& instance, const std::string& job_id, const std::string& result_digest,
                          const std::string& spec, const std::filesystem::path& directory,
                          const std::vector<types::PortType>& outputs, const WorkerProc::LogSink& log,
                          const WorkerProc::DisplaySink& display, const std::atomic<bool>& stop) -> RemoteInvocationResult;

}  // namespace tsugumi
