// Where a Card invocation runs. The scheduler resolves the target before
// evaluating the Card, then hands the derivation and invocation to the service
// the scheduler selected; it validates, records and routes what comes back. A
// backend resolves its target (prepare) and runs the invocation (execute):
// LocalExecutionService borrowing a connection from local tsugumi-workerd,
// RemoteExecutionService on a remote execution daemon with durable jobs.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "model.hh"
#include "nix.hh"
#include "tsugumi/workerd/local.hh"
#include "tsugumi/workerd/log_limits.hh"
#include "tsugumi/workerd/worker_execution.hh"

namespace tsugumi {

class Service;

namespace sql {
class Db;
}

struct ExecutionResult {
  std::vector<Value> outputs;
  std::optional<FailureData> failure;
  // Completed remote build and journal entries commit with the invocation.
  std::vector<std::pair<LogStream, std::string>> auxiliary_logs = {};
  bool auxiliary_logs_truncated = false;
};

// One invocation handed to a backend.
struct ExecutionRequest {
  const Node& node;
  const CardBuild& build;
  const std::vector<Value>& inputs;
  const std::vector<InputSourceData>& sources;
  // The invocation's live summary (run, Card, invocation number). A backend
  // fills its execution fields before start() and as they change.
  InvocationSummaryData& summary;
  // The invocation's scratch folder; out/ contains only declared outputs.
  std::filesystem::path directory;
  // Set when the run is cancelled or interrupted.
  const std::atomic<bool>& cancel;
  // An explicit run cancellation or a failed sibling asks the backend to
  // cancel submitted work. Shutdown stops waiting; startup records
  // interruption.
  std::function<bool()> cancel_requested;
  // Set when the process leaves the run while its invocations may still run
  // elsewhere (shutdown).
  std::stop_token detach;
  WorkerProc::LogSink log;
  WorkerProc::DisplaySink display;
  // Called with the call's begin and end (ms) right after the call answers.
  std::function<void(int64_t, int64_t)> on_timing;
  // Enters the execute stage: records the summary durably in one transaction
  // with the backend's own `records`, publishes it and marks the run running.
  // Called once, before anything of the Card can run.
  std::function<void(const std::function<void(sql::Db&)>& records)> start;
  // Publishes a change of the summary after start().
  std::function<void()> publish;
  // Native residents are reusable only after the scheduler accepts all
  // required outputs, their types and their paths. Throws Failed on rejection.
  std::function<void(workerd::InvocationResult&)> validate = {};
  uint64_t log_limit_bytes = kInvocationLogLimitBytes;
  std::function<void(std::string_view, std::string_view, uint64_t, uint64_t)>
      transfer = {};
  std::function<void(int64_t, int64_t)> on_remote_validation = {};
  std::stop_token stop = {};
};

// Thrown by execute() when shutdown stops waiting for remote execution.
// Startup completes its record as interrupted.
struct ExecutionDetached {};

// A backend's preparation for one activation. Kept until all its invocations
// finish (it may hold files its build wrote).
class PreparedExecution {
 public:
  virtual ~PreparedExecution() = default;
  // Make the derivation closure available before starting any invocation.
  virtual auto stage(const std::atomic<bool>&) -> void {}
  // Runs the invocation. Throws Failed when it cannot be started.
  virtual auto execute(const ExecutionRequest& request) -> ExecutionResult = 0;
};

#ifdef __APPLE__
auto prepare_fast_pack_execution(Service& service, const Node& node)
    -> std::unique_ptr<PreparedExecution>;
auto load_fast_pack_images(Service& service, std::span<const std::string> libraries)
    -> void;
auto stop_fast_pack_images(Service& service) -> void;
#endif

class LocalExecutionService final {
 public:
  LocalExecutionService(Service& service, std::filesystem::path scratch_root);

  auto prepare(const Node& node, const std::filesystem::path& directory,
               BuildRequest& build) -> std::unique_ptr<PreparedExecution>;
  // Runs the invocation directly on the connection workerd lends this Card.
  auto run(const ExecutionRequest& request) -> ExecutionResult;
  // Ends every worker before the build cache is cleared.
  auto clear() -> void;
  auto invalidate(const std::string& card, const std::string& reason) -> void;

 private:
  Service& service_;
  workerd::LocalWorkers workers_;
  std::mutex secrets_mutex_;
  std::string secrets_salt_;
  std::map<std::string, std::string> secret_versions_;
#ifdef __linux__
  std::mutex closure_mutex_;
  std::map<std::string, std::vector<std::string>> worker_closures_;
#endif
};

}  // namespace tsugumi
