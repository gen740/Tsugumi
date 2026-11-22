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

auto copy_output(int source, const stdfs::path& relative,
                 const stdfs::path& destination, PortKind kind,
                 const std::atomic<bool>& cancel, OutputDelivery delivery,
                 const fs::TransferProgress& progress, fs::OutputBudget& budget)
    -> void {
  try {
    fs::copy_staging_output(source, relative, destination,
                            kind == PortKind::Directory, cancel, progress,
                            &budget);
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

auto stage_input(const Value& value, std::span<const stdfs::path> roots,
                 const stdfs::path& directory, bool read_only,
                 const std::atomic<bool>& cancel,
                 const fs::TransferProgress& progress) -> Value {
  const auto source = path_value(value);
  if (!source)
    throw Failed(FailureKind::output_invalid, "file input has no path");
  for (const auto& root : roots) {
    const auto relative =
        stdfs::path(*source).lexically_normal().lexically_relative(root);
    if (!fs::valid_relative_path(relative.generic_string())) continue;
    const auto check_cancel = [&] {
      if (cancel.load())
        throw Failed(FailureKind::cancelled, "staging inputs cancelled");
    };
    const auto stage = [&](const stdfs::path& from, const stdfs::path& to) {
      check_cancel();
      if (read_only)
        if (const auto size = fs::link_beneath(root, from, to)) {
          if (progress) progress(from, *size, *size);
          return;
        }
      const auto total = stdfs::file_size(root / from);
      uint64_t done = 0;
      if (progress) progress(from, 0, total);
      fs::copy_beneath(root, from, to, [&](std::string_view chunk) {
        check_cancel();
        done += chunk.size();
        if (progress) progress(from, done, total);
      });
    };
    const auto destination = directory / relative.filename();
    if (value.kind == PortKind::File) {
      stage(relative, destination);
    } else {
      const auto entries = fs::list_beneath(root, relative);
      if (!entries.links.empty() || !entries.special.empty())
        throw Failed(FailureKind::output_invalid,
                     "Directory input contains a link or special file");
      stdfs::create_directories(destination);
      for (const auto& dir : entries.directories) {
        check_cancel();
        stdfs::create_directories(destination / dir);
      }
      for (const auto& file : entries.files)
        stage(relative / file, destination / file);
    }
    check_cancel();
    return with_path(value, destination.string());
  }
  throw Failed(FailureKind::output_invalid,
               "file input is outside the run's folders: " + *source);
}

auto accept_outputs(std::vector<std::pair<std::string, std::string>> raw,
                    std::span<const types::PortType> ports)
    -> std::vector<Value> {
  std::set<std::string_view> seen;
  std::vector<Value> outputs;
  outputs.reserve(raw.size());
  for (auto& [name, bytes] : raw) {
    const auto port = std::ranges::find(ports, name, &types::PortType::name);
    if (port == ports.end() || !port->type)
      throw Failed(FailureKind::output_invalid, "unknown output " + name,
                   {{"port", name}});
    if (!seen.insert(name).second)
      throw Failed(FailureKind::output_invalid,
                   "output " + name + " produced twice", {{"port", name}});
    if (!types::verify(*port->type, bytes))
      throw Failed(
          FailureKind::output_invalid,
          "output " + name + " is not a valid " + port->type->root_type,
          {{"port", name}});
    outputs.push_back({name, port->kind, port->type, std::move(bytes), {}});
  }
  for (const auto& port : ports)
    if (!port.optional && !seen.contains(port.name))
      throw Failed(FailureKind::output_invalid,
                   "required output " + port.name + " is missing",
                   {{"port", port.name}});
  return outputs;
}

auto stage_outputs(
    std::span<Value> outputs, std::span<const Value> inputs,
    std::span<const Value> original_inputs,
    std::span<const std::pair<std::string, std::string>> forwards,
    int work_root, const stdfs::path& work_path,
    const stdfs::path& input_directory, const stdfs::path& destination,
    OutputDelivery delivery, const std::atomic<bool>& cancel,
    const fs::TransferProgress& progress, fs::OutputBudget budget) -> void {
  const auto local = delivery == OutputDelivery::local_paths;
  for (auto& value : outputs) {
    const auto forward = std::ranges::find_if(
        forwards, [&](const auto& entry) { return entry.first == value.name; });
    if (value.kind == PortKind::FlatBuffer && forward == forwards.end())
      continue;
    const auto invalid = [&](std::string message) {
      return Failed(FailureKind::output_invalid, std::move(message),
                    {{"port", value.name}});
    };
    if (value.kind == PortKind::FlatBuffer)
      throw invalid("a value output cannot forward a file");
    const auto actual = path_value(value);
    if (!actual || actual->empty() || actual->find('\0') != std::string::npos)
      throw invalid("output has no valid path");
    if (forward != forwards.end()) {
      const auto input =
          std::ranges::find(inputs, forward->second, &Value::name);
      if (input == inputs.end() || input->kind != value.kind ||
          path_value(*input) != actual)
        throw invalid("output does not match the forwarded input");
      if (local) {
        const auto index = static_cast<size_t>(input - inputs.begin());
        const auto original = path_value(original_inputs[index]);
        if (!original) throw invalid("forwarded input has no original path");
        value = with_path(value, *original);
      } else {
        const auto relative =
            remote::relative_path(stdfs::path(*actual)
                                      .lexically_relative(input_directory)
                                      .generic_string());
        if (*relative.begin() != "inputs")
          throw invalid("remote forward is outside input staging");
        value = with_path(value, relative.generic_string());
      }
      continue;
    }

    const stdfs::path path(*actual);
    auto relative = path;
    if (path.is_absolute()) {
      relative = path.lexically_relative(work_path);
#ifdef __APPLE__
      if ((relative.empty() || *relative.begin() == "..") &&
          (actual->starts_with("/tmp/") || actual->starts_with("/var/")))
        relative =
            stdfs::path("/private" + *actual).lexically_relative(work_path);
#endif
    }
    if (relative.empty() || relative.is_absolute() ||
        std::ranges::any_of(relative,
                            [](const auto& part) { return part == ".."; }))
      throw invalid("output path is outside the worker's working directory");

    copy_output(work_root, relative, destination / relative, value.kind, cancel,
                delivery, progress, budget);
    const auto root = local ? destination : stdfs::path("out");
    value =
        with_path(value, (root / relative).lexically_normal().generic_string());
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
                           validation_rejected, result.failure);
    // Failed/dead/uncertain workers end without inspecting scratch. Transfer
    // failures retain living residents after clearing disposable call staging.
    if (between_calls && !validation_rejected && !result.failure &&
        worker.alive())
      worker.clear_scratch();
    release_worker(std::move(lease.worker), disposition.keep,
                   disposition.reason);
  };
  const auto discard_outputs = [&] {
    result.outputs.clear();
    if (invocation.output_directory.empty()) return;
    std::error_code error;
    stdfs::remove_all(invocation.output_directory, error);
  };
  try {
    worker.set_display(invocation.display);
    const bool needs_work = std::ranges::any_of(
        invocation.outputs,
        [](const auto& port) { return port.kind != PortKind::FlatBuffer; });
    std::optional<StagingHandle> work;
    stdfs::path work_path;
    if (needs_work) {
      work.emplace(worker.work_dir(), "worker working");
      // getcwd can return a physical /private path on Darwin.
      work_path = stdfs::canonical(worker.work_dir());
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
    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (invocation.timeout_ms)
      deadline = std::chrono::steady_clock::now() +
                 std::chrono::milliseconds(*invocation.timeout_ms +
                                           invocation.deadline_grace.count());
    const auto id = worker.next_call_id();
    auto frame =
        worker_call_frame(id, inputs, params.bfbs, params.root, params.bytes,
                          invocation.outputs, invocation.timeout_ms);
    worker.set_logs(invocation.log);
    const auto begin = invocation.on_timing ? now_ms() : 0;
    between_calls = false;
    auto outcome =
        worker.call(std::move(frame), id, invocation.cancel, deadline);
    result.failure = std::move(outcome.failure);
    between_calls = true;
    worker.set_logs(nullptr);
    if (invocation.on_timing) invocation.on_timing(begin, now_ms());
    if (!result.failure) {
      try {
        result.outputs =
            accept_outputs(std::move(outcome.outputs), invocation.outputs);
      } catch (const Failed& error) {
        result.failure = error.failure;
      } catch (...) {
        validation_rejected = true;
        throw;
      }
    }
    if (!result.failure) {
      const std::atomic<bool> no_cancel{false};
      stage_outputs(result.outputs, inputs, invocation.inputs, outcome.forwards,
                    work ? work->get() : -1, work_path,
                    invocation.input_directory, invocation.output_directory,
                    invocation.delivery,
                    invocation.cancel ? *invocation.cancel : no_cancel,
                    invocation.transfer, invocation.output_budget);
      if (invocation.validate) {
        try {
          invocation.validate(result);
        } catch (const Failed& error) {
          result.failure = error.failure;
        } catch (...) {
          validation_rejected = true;
          throw;
        }
      }
    }
    if (result.failure) discard_outputs();
    try {
      release();
    } catch (...) {
      if (!result.failure) throw;
      return result;
    }
  } catch (...) {
    discard_outputs();
    try {
      if (lease.worker) release();
    } catch (...) {
    }
    throw;
  }
  return result;
}

}  // namespace tsugumi::workerd
