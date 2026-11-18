// One invocation over an acquired worker connection. The caller supplies run
// decisions and storage adapters; this layer owns only execution mechanisms.
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "tsugumi/workerd/staging.hh"
#include "tsugumi/workerd/worker_pool.hh"

namespace tsugumi::workerd {

// Preserve the existing carriers' output semantics until storage handover is
// unified. Local forwards original input paths and defers validation to core;
// transferred results copy forwarded inputs and use relative transport paths.
enum class OutputDelivery { local_paths, transferred_files };

struct InvocationResult {
  WorkerOutcome outcome;
  // A carrier may return logs with the result instead of streaming them.
  std::vector<std::pair<LogStream, std::string>> logs;
  bool log_truncated = false;
  // Worker-visible out path and its caller-visible destination.
  std::optional<std::pair<std::string, std::string>> out_alias;
};

struct InvocationParameters {
  std::string bfbs;
  std::string root;
  std::string bytes;
  std::optional<std::string> json;
};

struct Invocation {
  std::string run_id;
  CardPath card;
  uint64_t number = 0;
  // Borrowed for this synchronous invocation; never retained by the pool.
  std::span<const Value> inputs;
  std::vector<types::PortType> outputs;
  InvocationParameters params;
  std::string input_schema;
  std::string output_schema;
  std::string path;
  std::string worker_binary;
  std::vector<std::pair<std::string, std::string>> environment;
  std::filesystem::path output_directory;
  std::optional<std::pair<uid_t, gid_t>> output_owner;
  OutputDelivery delivery = OutputDelivery::local_paths;
  // The remote input snapshot root. Local input copies are made by
  // stage_inputs.
  std::filesystem::path input_directory;
  const std::atomic<bool>* cancel = nullptr;
  std::optional<uint64_t> timeout_ms;
  std::chrono::milliseconds deadline_grace{0};
  // Remote workers used to stop before output inspection, with the supervisor
  // enforcing the whole-job deadline/cancel outside this call.
  bool stop_before_outputs = false;
  WorkerProc::LogSink log;
  WorkerProc::DisplaySink display;
  std::function<std::vector<Value>(WorkerProc&)> stage_inputs;
  std::function<InvocationParameters()> prepare_params;
  std::function<void(int64_t, int64_t)> on_timing;
  std::function<void(const InvocationResult&)> validate;
  fs::TransferProgress transfer = {};
};

// Exactly one decision for resident retention, including exceptional exits.
// Transfer failures occur between calls; protocol uncertainty and rejected
// required outputs do not. The pool additionally checks process liveness.
struct WorkerDisposition {
  bool keep;
  std::string reason;
};
auto worker_disposition(bool reuse, bool alive, bool between_calls,
                        bool validation_rejected,
                        const std::optional<FailureData>& failure)
    -> WorkerDisposition;

// Output transfer can fail after a successful ABI call. It must not turn that
// call into a failed Card or discard a resident. Validation is a separate step.
auto stage_outputs(WorkerOutcome& outcome,
                   const std::vector<types::PortType>& ports,
                   std::span<const Value> inputs,
                   std::span<const Value> original_inputs, int output_root,
                   const std::string& output_alias, int input_root,
                   const std::filesystem::path& input_directory,
                   const std::filesystem::path& destination,
                   OutputDelivery delivery, const std::atomic<bool>& cancel,
                   const fs::TransferProgress& progress = {}) -> void;

using ReleaseWorker =
    std::function<void(std::unique_ptr<WorkerProc>, bool, const std::string&)>;

auto run_invocation(const ReleaseWorker& release_worker,
                    WorkerPool::Lease& lease, bool reuse,
                    const Invocation& invocation) -> InvocationResult;

}  // namespace tsugumi::workerd
