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

struct InvocationResult {
  std::vector<Value> outputs;
  std::optional<FailureData> failure;
  std::vector<ArtifactKey> created = {};
};

struct Invocation {
  // Borrowed for this synchronous invocation; never retained by the pool.
  std::span<const Value> inputs;
  std::vector<types::PortType> outputs;
  std::filesystem::path artifact_directory;
  uint64_t artifact_sequence = 0;
  std::vector<ArtifactGrant> grants;
  fs::OutputBudget output_budget;
  const std::atomic<bool>* cancel = nullptr;
  std::optional<uint64_t> timeout_ms;
  std::chrono::milliseconds deadline_grace{0};
  WorkerProc::LogSink log;
  WorkerProc::DisplaySink display;
  std::function<void(int64_t, int64_t)> on_timing;
  std::function<void(InvocationResult&)> validate;
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

// Admit opaque outputs by declared name, required presence and capability.
auto accept_outputs(std::vector<RawOutput> raw,
                    std::span<const types::PortType> ports,
                    std::span<const Value> inputs = {},
                    std::span<const ArtifactKey> created = {})
    -> std::vector<Value>;

using ReleaseWorker =
    std::function<void(std::unique_ptr<WorkerProc>, bool, const std::string&)>;

auto run_invocation(const ReleaseWorker& release_worker,
                    WorkerPool::Lease& lease, bool reuse,
                    const Invocation& invocation) -> InvocationResult;

}  // namespace tsugumi::workerd
