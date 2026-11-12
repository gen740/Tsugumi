#pragma once

#include "execution_service.hh"
#include "remote_job.hh"
#include "schema.hh"

namespace tsugumi {

namespace remote {

struct FileJson {
  schema::t::String<"path"> path;
  schema::t::String<"sha256"> sha256;
  schema::t::Uint64<"size"> size;
};

struct InvocationJson {
  schema::t::String<"project_id"> project_id;
  schema::t::String<"call_sha256"> call_sha256;
  schema::t::String<"archive_sha256"> archive_sha256;
  schema::t::Array<"files", schema::Obj<FileJson>> files;
  schema::t::Array<"directories", schema::v::String> directories;
  schema::t::Bool<"network"> network;
  schema::t::Bool<"filesystem"> filesystem;
  schema::t::Uint64<"implementation_index"> implementation_index;
};

struct JobJson {
  schema::t::String<"system"> system;
  schema::t::String<"derivation"> derivation;
  schema::t::String<"output"> output;
  schema::t::String<"invocation_sha256"> invocation_sha256;
  schema::t::Array<"input_blobs", schema::v::String> input_blobs;
  schema::t::Uint64<"cpu_millis"> cpu_millis;
  schema::t::Uint64<"memory_bytes"> memory_bytes;
  schema::t::Uint64<"timeout_seconds"> timeout_seconds;
};

struct ResultFileJson {
  schema::Opt<"path", schema::v::String> path;
  schema::Opt<"sha256", schema::v::String> sha256;
  schema::Opt<"size", schema::v::Uint64> size;
};

struct ResultJson {
  // Network JSON object order is immaterial. Check required keys after
  // decoding so the order-sensitive constant-field decoder is not used.
  schema::Opt<"invocation_result_sha256", schema::v::String> invocation_result_sha256;
  schema::Opt<"outputs", schema::v::Array<schema::Obj<ResultFileJson>>> outputs;
};

struct EnvironmentJson {
  schema::t::Array<"paths", schema::v::String> paths;
  schema::t::Array<"directories", schema::v::String> directories;
  schema::t::Bool<"built"> built;
};

struct LogJson {
  schema::t::Uint64<"stream"> stream;
  schema::t::String<"text"> text;
};

auto relative_path(std::string_view path) -> std::filesystem::path;
auto store_path(std::string_view path) -> bool;

}  // namespace remote

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

auto stage_remote_archive(const RemoteJobClient& client, const std::string& service, const std::string& instance,
                          const std::filesystem::path& archive, const std::atomic<bool>& cancel) -> std::string;

auto stage_remote_invocation(const RemoteJobClient& client, const std::string& service, const std::string& instance,
                             const std::string& project_id,
                             const std::filesystem::path& state, const std::string& archive_digest,
                             const LocalExecutionRequest& request) -> std::string;
auto import_remote_result(const RemoteJobClient& client, const std::string& service, const std::string& instance,
                          const std::string& result_digest,
                          const std::string& spec, const std::filesystem::path& directory,
                          const std::vector<types::PortType>& outputs, const WorkerProc::LogSink& log,
                          const WorkerProc::DisplaySink& display, const std::atomic<bool>& stop) -> RemoteInvocationResult;

}  // namespace tsugumi
