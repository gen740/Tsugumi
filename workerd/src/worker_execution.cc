#include "tsugumi/workerd/worker_execution.hh"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <set>
#include <stdexcept>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/workerd/invocation.hh"
#include "tsugumi/workerd/protocol.hh"
#include "tsugumi/workerd/staging.hh"

namespace tsugumi::workerd {
namespace stdfs = std::filesystem;

namespace {
// Pin roots before Card code runs, then traverse relative to trusted handles.
class StagingHandle {
 public:
  StagingHandle(const stdfs::path& path, const char* location)
      : fd_(::open(path.c_str(),
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) {
    if (fd_ < 0)
      throw Failed(
          FailureKind::output_invalid,
          std::string("cannot open ") + location + " staging directory");
  }
  ~StagingHandle() { ::close(fd_); }
  StagingHandle(const StagingHandle&) = delete;
  auto get() const -> int { return fd_; }

 private:
  int fd_;
};

}  // namespace

auto worker_disposition(bool reuse, bool alive, bool between_calls,
                        bool validation_rejected,
                        const std::optional<FailureData>& failure)
    -> WorkerDisposition {
  if (validation_rejected)
    return {false, "the previous call failed validation"};
  if (!between_calls) return {false, "the previous call failed"};
  if (failure) {
    const auto kind = failure->kind;
    return {false, kind == FailureKind::cancelled
                       ? "the previous call was cancelled"
                   : kind == FailureKind::timeout
                       ? "the previous call exceeded its deadline"
                   : alive ? "the previous call failed"
                           : "the resident worker died"};
  }
  return {reuse && alive, {}};
}

auto accept_outputs(std::vector<RawOutput> raw,
                    std::span<const types::PortType> ports,
                    std::span<const Value> inputs,
                    std::span<const ArtifactKey> created)
    -> std::vector<Value> {
  std::set<ArtifactKey> allowed;
  for (const auto& value : inputs)
    allowed.insert(value.artifacts.begin(), value.artifacts.end());
  for (const auto& id : created)
    if (id.size() != 16 || !allowed.insert(id).second)
      throw Failed(FailureKind::output_invalid, "invalid created artifact id");
  std::set<std::string_view> seen;
  std::vector<Value> outputs;
  outputs.reserve(raw.size());
  for (auto& output : raw) {
    const auto& name = output.name;
    const auto port = std::ranges::find(ports, name, &types::PortType::name);
    if (port == ports.end())
      throw Failed(FailureKind::output_invalid, "unknown output " + name,
                   {{"port", name}});
    if (!seen.insert(name).second)
      throw Failed(FailureKind::output_invalid,
                   "output " + name + " produced twice", {{"port", name}});
    for (const auto& id : output.artifacts)
      if (!allowed.contains(id))
        throw Failed(FailureKind::output_invalid,
                     "output references an ungranted artifact", {{"port", name}});
    outputs.push_back({name, port->type, std::move(output.bytes), {},
                       std::move(output.artifacts)});
  }
  for (const auto& port : ports)
    if (port.required && !seen.contains(port.name))
      throw Failed(FailureKind::output_invalid,
                   "required output " + port.name + " is missing",
                   {{"port", port.name}});
  return outputs;
}

auto run_invocation(const ReleaseWorker& release_worker,
                    WorkerPool::Lease& lease, bool reuse,
                    const Invocation& invocation) -> InvocationResult {
  WorkerProc& worker = *lease.worker;
  InvocationResult result;
  bool between_calls = true;
  bool validation_rejected = false;
  const auto release = [&] {
    const auto disposition =
        worker_disposition(reuse, worker.alive(), between_calls,
                           validation_rejected, result.failure);
    // Failed/dead/uncertain workers end without inspecting scratch. Transfer
    // failures retain living residents after clearing disposable call staging.
    if (between_calls && !validation_rejected && !result.failure &&
        worker.alive())
      worker.clear_scratch();
    release_worker(std::move(lease.worker), disposition.keep,
                   disposition.reason);
  };
  try {
    worker.set_display(invocation.display);
    std::optional<StagingHandle> stage;
    stdfs::path stage_path;
    if (!invocation.artifact_directory.empty()) {
      // Pin the trusted staging directory before Card code can change it.
      stage_path = stdfs::path(worker.tmp_dir()) / ("artifacts-" + crypto::uuid4());
      stdfs::create_directory(stage_path);
      stdfs::permissions(stage_path,
                         stdfs::perms::owner_all | stdfs::perms::group_all |
                             stdfs::perms::others_all);
      stage.emplace(stage_path, "artifact");
    }
    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (invocation.timeout_ms)
      deadline = std::chrono::steady_clock::now() +
                 std::chrono::milliseconds(*invocation.timeout_ms +
                                           invocation.deadline_grace.count());
    const auto id = worker.next_call_id();
    auto frame = worker_call_frame(id, invocation.inputs, invocation.outputs,
                                   invocation.timeout_ms, stage_path,
                                   invocation.grants);
    worker.set_logs(invocation.log);
    const auto begin = invocation.on_timing ? now_ms() : 0;
    between_calls = false;
    auto outcome =
        worker.call(std::move(frame), id, invocation.cancel, deadline);
    result.failure = std::move(outcome.failure);
    between_calls = true;
    worker.set_logs(nullptr);
    if (invocation.on_timing) invocation.on_timing(begin, now_ms());
    std::optional<FailureData> transfer_failure;
    try {
      if (!outcome.created.empty() && !stage)
        throw Failed(FailureKind::output_invalid, "unexpected artifact creation");
      auto budget = invocation.output_budget;
      // Once a call has returned, even artifacts omitted from its result
      // belong to the run. Preserve artifacts from a reported Card failure.
      const std::atomic<bool> persist{false};
      std::set<ArtifactKey> seen;
      for (const auto& id : outcome.created) {
        if (id.size() != 16 || !seen.insert(id).second)
          throw Failed(FailureKind::output_invalid, "invalid created artifact id");
        const auto name = crypto::hex(id.data(), id.size());
        const auto destination = invocation.artifact_directory / name;
        try {
          fs::copy_staging_output(stage->get(), name, destination,
                                  persist, invocation.transfer, &budget);
          if (!stdfs::is_directory(stdfs::symlink_status(destination)))
            throw fs::Error("artifact must be a directory");
        } catch (const std::runtime_error& error) {
          std::error_code cleanup_error;
          stdfs::remove_all(destination, cleanup_error);
          transfer_failure =
              FailureData::make(FailureKind::output_invalid, error.what());
          break;
        }
        result.created.push_back(id);
      }
      if (!result.failure && !transfer_failure) {
        result.outputs = accept_outputs(std::move(outcome.outputs),
                                        invocation.outputs, invocation.inputs,
                                        outcome.created);
        if (invocation.validate) invocation.validate(result);
      }
    } catch (const Failed& error) {
      result.failure = error.failure;
    } catch (const fs::Error& error) {
      result.failure = FailureData::make(FailureKind::output_invalid, error.what());
    }
    if (result.failure) result.outputs.clear();
    try {
      release();
    } catch (...) {
      if (!result.failure && !transfer_failure) throw;
    }
    // A rejected transfer leaves a successful resident call reusable.
    if (transfer_failure) result.failure = std::move(transfer_failure);
  } catch (...) {
    try {
      if (lease.worker) release();
    } catch (...) {
    }
    throw;
  }
  return result;
}

}  // namespace tsugumi::workerd
