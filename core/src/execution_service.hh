// Where a Card invocation runs. The scheduler plans and builds the Card,
// then hands the invocation to the execution service the Card names; it
// validates, records and routes what comes back. A backend configures the
// build it needs (prepare) and runs the invocation (execute):
// LocalExecutionService on a local tsugumi-worker process,
// RemoteExecutionService on a remote execution daemon with durable jobs.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model.hh"
#include "nix.hh"
#include "worker_pool.hh"

namespace tsugumi {

class Service;

namespace sql {
class Db;
}

// One invocation handed to a backend.
struct ExecutionRequest {
  const Node& node;
  const CardBuild& build;
  const std::vector<Value>& inputs;
  const std::vector<InputSourceData>& sources;
  // The invocation's live summary (run, Card, invocation number). A backend
  // fills its execution fields before start() and as they change.
  InvocationSummaryData& summary;
  // The invocation's folder in the run (out/, and work/ and tmp/ copies for
  // filesystem: true).
  std::filesystem::path directory;
  // Set when the run is cancelled or interrupted.
  const std::atomic<bool>& cancel;
  // Set by an explicit cancellation of the run (not by a shutdown).
  const std::atomic<bool>& cancel_requested;
  // Set when the process leaves the run while its invocations may still run
  // elsewhere (shutdown).
  const std::atomic<bool>& detach;
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
};

struct ExecutionResult {
  WorkerOutcome outcome;
  // Logs returned with the result instead of streamed through `log`; they are
  // recorded in the same transaction as the invocation's completion.
  std::vector<std::pair<LogStream, std::string>> logs;
  // The (descriptor path, real path) of out/ when the folder reached the Card
  // as a descriptor.
  std::optional<std::pair<std::string, std::string>> out_alias;
};

// Thrown by execute() when the process detaches from an invocation that keeps
// running in its backend. Recovery completes its record later.
struct ExecutionDetached {};

// An invocation whose backend-specific preparation is done. Kept until the
// invocation ends (it may hold files its build wrote).
class PreparedExecution {
 public:
  virtual ~PreparedExecution() = default;
  // Runs the invocation. Throws Failed when it cannot be started.
  virtual auto execute(const ExecutionRequest& request) -> ExecutionResult = 0;
};

class ExecutionService {
 public:
  virtual ~ExecutionService() = default;
  // Whether a build made earlier in the run for a Card serves its later
  // invocations.
  virtual auto reuses_builds() const -> bool = 0;
  // Checks that the invocation can run here and configures the backend's
  // part of `build` (target system, derivation export). Called before the
  // Card is built, for every invocation.
  virtual auto prepare(const Node& node, const std::filesystem::path& directory, BuildRequest& build)
      -> std::unique_ptr<PreparedExecution> = 0;
};

class LocalExecutionService final : public ExecutionService {
 public:
  LocalExecutionService(Service& service, std::filesystem::path scratch_root);

  auto reuses_builds() const -> bool override { return true; }
  auto prepare(const Node& node, const std::filesystem::path& directory, BuildRequest& build)
      -> std::unique_ptr<PreparedExecution> override;
  // Runs the invocation on a worker of its Card.
  auto run(const ExecutionRequest& request) -> ExecutionResult;
  // Ends every worker before the build cache is cleared.
  auto clear() -> void;

 private:
  Service& service_;
  WorkerPool workers_;
};

}  // namespace tsugumi
