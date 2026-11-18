#include "tsugumi/workerd/worker_execution.hh"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/workerd/invocation.hh"
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

auto copy_output(int source, const stdfs::path& relative,
                 const stdfs::path& destination, PortKind kind,
                 const std::atomic<bool>& cancel, OutputDelivery delivery,
                 const fs::TransferProgress& progress) -> void {
  try {
    fs::copy_staging_output(source, relative, destination,
                            kind == PortKind::Directory, cancel, progress);
  } catch (const std::exception& error) {
    if (delivery == OutputDelivery::local_paths && cancel.load())
      throw Failed(FailureKind::cancelled, "output transfer cancelled");
    throw Failed(FailureKind::output_invalid, error.what());
  }
}
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

auto stage_outputs(WorkerOutcome& outcome,
                   const std::vector<types::PortType>& ports,
                   std::span<const Value> inputs,
                   std::span<const Value> original_inputs, int output_root,
                   const std::string& output_alias, int input_root,
                   const stdfs::path& input_directory,
                   const stdfs::path& destination, OutputDelivery delivery,
                   const std::atomic<bool>& cancel,
                   const fs::TransferProgress& progress) -> void {
  if (delivery == OutputDelivery::local_paths) {
    // Forwarded inputs retain their recorded path, not worker staging paths.
    for (auto& [name, bytes] : outcome.outputs) {
      const auto port = std::ranges::find(ports, name, &types::PortType::name);
      if (port == ports.end() || port->kind == PortKind::FlatBuffer ||
          !types::verify(*port->type, bytes))
        continue;
      Value value{name, port->kind, port->type, bytes, {}};
      for (size_t i = 0; i < inputs.size(); ++i)
        if (inputs[i].kind == value.kind &&
            path_value(inputs[i]) == path_value(value)) {
          bytes = with_path(value, *path_value(original_inputs[i])).bytes;
          break;
        }
    }
  }
  if (outcome.failure || destination.empty()) return;
  stdfs::path forwarded;
  for (auto& [name, bytes] : outcome.outputs) {
    const auto port = std::ranges::find(ports, name, &types::PortType::name);
    if (port == ports.end() || !types::verify(*port->type, bytes)) {
      if (delivery == OutputDelivery::local_paths) continue;
      throw Failed(FailureKind::output_invalid,
                   "invalid remote worker output " + name);
    }
    if (port->kind == PortKind::FlatBuffer) continue;
    Value value{name, port->kind, port->type, bytes, {}};
    const auto actual = path_value(value);
    if (!actual) {
      if (delivery == OutputDelivery::local_paths) continue;
      throw Failed(FailureKind::output_invalid, "remote output has no path");
    }
    if (delivery == OutputDelivery::local_paths) {
      const auto relative =
          stdfs::path(*actual).lexically_relative(output_alias);
      if (relative.empty() || relative.is_absolute() ||
          std::ranges::any_of(relative,
                              [](const auto& part) { return part == ".."; }))
        continue;
      copy_output(output_root, relative, destination / relative, port->kind,
                  cancel, delivery, progress);
      continue;
    }
    stdfs::path rel;
    if (*actual == output_alias) {
      rel = "out";
      copy_output(output_root, ".", destination, port->kind, cancel, delivery,
                  progress);
    } else if (actual->starts_with(output_alias + "/")) {
      const auto relative = remote::relative_path(
          std::string_view(*actual).substr(output_alias.size() + 1));
      rel = "out" / relative;
      copy_output(output_root, relative, destination / relative, port->kind,
                  cancel, delivery, progress);
    } else if (std::ranges::any_of(inputs, [&](const Value& in) {
                 return in.kind == port->kind && path_value(in) == actual;
               })) {
      if (forwarded.empty()) {
        forwarded = stdfs::path("out") / (".forwarded-" + crypto::uuid4());
        if (!stdfs::create_directories(destination.parent_path() / forwarded))
          throw Failed(FailureKind::output_invalid,
                       "cannot create a private forwarded output directory");
      }
      rel = forwarded / std::to_string(port - ports.begin()) /
            stdfs::path(*actual).filename();
      copy_output(input_root,
                  stdfs::path(*actual).lexically_relative(input_directory),
                  destination.parent_path() / rel, port->kind, cancel, delivery,
                  progress);
    } else {
      throw Failed(FailureKind::output_invalid,
                   "remote output points outside its output directory");
    }
    const auto actual_path = destination.parent_path() / rel;
    if (!fs::resolves_inside(destination, actual_path) &&
        actual_path != destination)
      throw Failed(FailureKind::output_invalid,
                   "remote output escapes its directory");
    bytes = with_path(value, rel.generic_string()).bytes;
  }
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
                           validation_rejected, result.outcome.failure);
    // Failed/dead/uncertain workers end without inspecting scratch. Transfer
    // failures retain living residents after clearing disposable call staging.
    if (between_calls && !validation_rejected && !result.outcome.failure &&
        worker.alive())
      worker.clear_scratch();
    release_worker(std::move(lease.worker), disposition.keep,
                   disposition.reason);
  };
  try {
    const bool local = invocation.delivery == OutputDelivery::local_paths;
    const bool needs_out = std::ranges::any_of(
        invocation.outputs,
        [](const auto& port) { return port.kind != PortKind::FlatBuffer; });
    if (local && needs_out) {
      stdfs::create_directories(invocation.output_directory);
      if (invocation.output_owner)
        if (::chown(invocation.output_directory.c_str(),
                    invocation.output_owner->first,
                    invocation.output_owner->second) != 0)
          throw std::runtime_error("cannot give the worker its output folder " +
                                   invocation.output_directory.string() + ": " +
                                   std::strerror(errno));
      result.out_alias = std::make_pair(worker.out_dir(),
                                        invocation.output_directory.string());
    }
    // Borrow unchanged inputs; only the local storage adapter makes a staged
    // vector. Large value buffers need no extra copy at this boundary.
    const auto staged = invocation.stage_inputs
                            ? invocation.stage_inputs(worker)
                            : std::vector<Value>{};
    const std::span<const Value> inputs =
        invocation.stage_inputs ? staged : invocation.inputs;
    const auto params = invocation.prepare_params ? invocation.prepare_params()
                                                  : invocation.params;
    std::vector<std::pair<std::string, std::string>> environment{
        {"PATH", invocation.path},
        {"HOME", worker.work_dir()},
        {"LANG", "C.UTF-8"},
        {"TSUGUMI_RUN_ID", invocation.run_id},
        {"TSUGUMI_CARD", path_key(invocation.card)},
        {"TSUGUMI_INVOCATION", std::to_string(invocation.number)},
    };
    // Keep the local environment order too (observable to a native Card).
    if (local && params.json)
      environment.emplace_back("TSUGUMI_PARAMS_JSON", *params.json);
    environment.emplace_back("TSUGUMI_WORKER", invocation.worker_binary);
    if (!local && params.json)
      environment.emplace_back("TSUGUMI_PARAMS_JSON", *params.json);
    environment.insert(environment.end(), invocation.environment.begin(),
                       invocation.environment.end());
    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (invocation.timeout_ms)
      deadline = std::chrono::steady_clock::now() +
                 std::chrono::milliseconds(*invocation.timeout_ms +
                                           invocation.deadline_grace.count());
    const auto id = worker.next_call_id();
    auto frame = worker_call_frame(
        id, invocation.run_id, invocation.card, invocation.number, inputs,
        params.bfbs, params.root, params.bytes, invocation.outputs,
        worker.work_dir(), worker.tmp_dir(), needs_out ? worker.out_dir() : "",
        environment, invocation.timeout_ms);
    worker.set_logs(invocation.log);
    StagingHandle output_directory(worker.out_dir(),
                                   local ? "worker" : "remote");
    std::optional<StagingHandle> input_directory;
    if (!local) input_directory.emplace(invocation.input_directory, "remote");
    const auto begin = invocation.on_timing ? now_ms() : 0;
    between_calls = false;
    result.outcome =
        worker.call(frame, id, invocation.cancel, deadline, invocation.display);
    between_calls = true;
    if (invocation.stop_before_outputs)
      worker.terminate();
    else
      worker.set_logs(nullptr);
    if (invocation.finish_logs) invocation.finish_logs();
    if (invocation.on_timing) invocation.on_timing(begin, now_ms());
    const std::atomic<bool> no_cancel{false};
    stage_outputs(
        result.outcome, invocation.outputs, inputs, invocation.inputs,
        output_directory.get(), worker.out_dir(),
        input_directory ? input_directory->get() : -1,
        invocation.input_directory,
        local && !needs_out ? stdfs::path{} : invocation.output_directory,
        invocation.delivery, invocation.cancel ? *invocation.cancel : no_cancel,
        invocation.transfer);
    if (!result.outcome.failure && invocation.validate) {
      try {
        invocation.validate(result);
      } catch (const Failed& error) {
        result.outcome.failure = error.failure;
      } catch (...) {
        validation_rejected = true;
        throw;
      }
    }
    try {
      release();
    } catch (...) {
      if (!result.outcome.failure) throw;
      return result;
    }
  } catch (...) {
    try {
      if (lease.worker) release();
    } catch (...) {
    }
    try {
      if (invocation.finish_logs) invocation.finish_logs();
    } catch (...) {
    }
    throw;
  }
  return result;
}

}  // namespace tsugumi::workerd
