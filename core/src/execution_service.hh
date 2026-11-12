// Runs one Card invocation on a local tsugumi-worker process: spawn
// conditions, the resident or standby worker, the call's folders and the
// call itself. The scheduler keeps planning, builds and output handling.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model.hh"
#include "nix.hh"
#include "worker_pool.hh"

namespace tsugumi {

class Service;

struct LocalExecutionRequest {
  const Node& node;
  const CardBuild& build;
  const std::vector<Value>& inputs;
  const std::string& run_id;
  uint64_t invocation;
  // The invocation's folder in the run (out/, and work/ and tmp/ copies for
  // filesystem: true).
  std::filesystem::path directory;
  const std::atomic<bool>& cancel;
  WorkerProc::LogSink log;
  WorkerProc::DisplaySink display;
  // Called with the call's begin and end (ms) right after the worker answers.
  std::function<void(int64_t, int64_t)> on_timing;
};

class LocalExecutionService {
 public:
  LocalExecutionService(Service& service, std::filesystem::path scratch_root);

  // Runs the invocation on a worker of its Card. `worker_reused` tells whether
  // a resident worker served it; `out_alias` is the (descriptor path, real
  // path) of out/ when the folder reached the Card as a descriptor.
  // Throws Failed when the call cannot be started.
  auto run(const LocalExecutionRequest& request, std::optional<bool>& worker_reused,
           std::optional<std::pair<std::string, std::string>>& out_alias) -> WorkerOutcome;
  // Ends every worker before the build cache is cleared.
  auto clear() -> void;

 private:
  Service& service_;
  WorkerPool workers_;
};

}  // namespace tsugumi
