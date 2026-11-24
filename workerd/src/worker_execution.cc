#include "tsugumi/workerd/worker_execution.hh"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <set>
#include <stdexcept>
#include <system_error>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/workerd/artifact_id.hh"
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
                    std::span<const types::PortType> ports)
    -> std::vector<Value> {
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
    outputs.push_back({name, port->type, std::move(output.bytes), {}});
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
    // Retiring a worker stops its process group before clearing scratch.
    // A resident keeps its sandbox: clearing its staging removes this call's
    // input and output paths from its view.
    if (disposition.keep) worker.clear_scratch();
    release_worker(std::move(lease.worker), disposition.keep,
                   disposition.reason);
  };
  try {
    worker.set_display(invocation.display);
    std::optional<StagingHandle> stage;
    stdfs::path stage_path;
    if (!invocation.artifact_directory.empty()) {
      // Pin the trusted staging directory before Card code can change it.
      stage_path = worker.artifact_scratch() / "outputs" /
                   std::to_string(invocation.artifact_sequence);
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
                                   invocation.timeout_ms, stage_path, !reuse);
    const auto open = [&](const ArtifactKey& key,
                          std::stop_token stop) -> std::string {
      if (!invocation.resolve_artifact) return {};
      const auto destination =
          worker.artifact_scratch() / "inputs" / artifact_id_text(key);
      try {
        if (!stdfs::is_directory(stdfs::symlink_status(destination))) {
          const auto source = invocation.resolve_artifact(key, stop);
          if (source.empty()) return {};
          fs::link_artifact(source, destination);
        }
        return destination.string();
      } catch (const std::exception&) {
        return {};
      }
    };
    worker.set_logs(invocation.log);
    const auto begin = invocation.on_timing ? now_ms() : 0;
    between_calls = false;
    auto outcome = worker.call(std::move(frame), id, invocation.cancel,
                               deadline, open, stage.has_value());
    result.failure = std::move(outcome.failure);
    between_calls = true;
    worker.set_logs(nullptr);
    if (invocation.on_timing) invocation.on_timing(begin, now_ms());
    std::optional<FailureData> transfer_failure;
    // A resident, or on macOS a detached child of the Card (there is no PID
    // namespace), can keep a writer on the staged inodes past the call. Seal
    // clones, which are new inodes, instead. Linux retires a fresh worker's
    // namespace and renames the staged tree.
#ifdef __APPLE__
    const bool clone = true;
#else
    const bool clone = reuse;
#endif
    struct Clones {
      stdfs::path path;
      ~Clones() {
        if (path.empty()) return;
        std::error_code ec;
        stdfs::remove_all(path, ec);
      }
    } clones;
    try {
      if (!outcome.created.empty() && !stage)
        throw Failed(FailureKind::output_invalid, "unexpected artifact creation");
      auto budget = invocation.output_budget;
      // A fresh worker is retired before the run takes over its outputs.
      if (!outcome.created.empty() && !reuse) worker.terminate(true);
      std::optional<StagingHandle> clones_handle;
      if (!outcome.created.empty() && clone) {
        // A sibling of the worker's staging: same volume, hidden from Cards.
        clones.path = worker.artifact_scratch().parent_path() / crypto::uuid4();
        stdfs::create_directory(clones.path);
        clones_handle.emplace(clones.path, "artifact clone");
      }
      for (const auto& artifact : outcome.created) {
        const auto name = std::to_string(result.created.size());
        try {
          if (clone) {
            fs::clone_staging_artifact(stage->get(), name, clones.path / name,
                                       budget);
            fs::move_staging_artifact(clones_handle->get(), name,
                                      invocation.artifact_directory / name,
                                      budget);
            // Clearing or retiring the worker removes whatever this cannot.
            try {
              fs::remove_staging_artifact(stage->get(), name);
            } catch (const std::system_error&) {
            }
          } else {
            fs::move_staging_artifact(stage->get(), name,
                                      invocation.artifact_directory / name,
                                      budget);
          }
        } catch (const std::runtime_error& error) {
          transfer_failure =
              FailureData::make(FailureKind::output_invalid, error.what());
          break;
        }
        result.created.push_back(artifact);
      }
      if (!result.failure && !transfer_failure) {
        result.outputs =
            accept_outputs(std::move(outcome.outputs), invocation.outputs);
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
    if (transfer_failure && !result.failure)
      result.failure = std::move(transfer_failure);
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
