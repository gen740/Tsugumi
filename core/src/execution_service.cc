#include "execution_service.hh"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <system_error>

#include "history.hh"
#include "run_scratch.hh"
#include "service.hh"
#include "store.hh"
#include "util/crypto.hh"
#include "util/staging.hh"
#include "tsugumi/workerd/log.hh"

#ifndef TSUGUMI_CARD_PATH
#define TSUGUMI_CARD_PATH "/usr/bin:/bin"
#endif
#ifndef TSUGUMI_BASH
#define TSUGUMI_BASH "/bin/bash"
#endif

namespace tsugumi {

namespace {

// The unprivileged identity for Card processes.
constexpr uid_t kCardUid = 65534;

// Open before calling the worker, then traverse relative to this trusted
// handle. A live Card child may rename entries at any point in the copy.
class StagingHandle {
 public:
  explicit StagingHandle(const std::filesystem::path& path)
      : StagingHandle(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) {}
  explicit StagingHandle(int fd) : fd_(fd) {
    if (fd_ < 0) throw Failed(FailureKind::output_invalid, "cannot open worker staging directory");
  }
  ~StagingHandle() { ::close(fd_); }
  StagingHandle(const StagingHandle&) = delete;
  auto get() const -> int { return fd_; }
 private:
  int fd_;
};

class PreparedNativeExecution final : public PreparedExecution {
 public:
  explicit PreparedNativeExecution(NativeExecutionService& service) : service_(service) {}
  auto execute(const ExecutionRequest& request) -> ExecutionResult override { return service_.run(request); }

 private:
  NativeExecutionService& service_;
};

}  // namespace

NativeExecutionService::NativeExecutionService(Service& service, std::filesystem::path /*scratch_root*/)
    : service_(service), workers_(private_disk_scratch(service.run_scratch() / "workers")) {}

auto NativeExecutionService::clear() -> void { workers_.clear(false); }

auto NativeExecutionService::prepare(const Node& node, const std::filesystem::path& /*directory*/,
                                    BuildRequest& build) -> std::unique_ptr<PreparedExecution> {
  static const SharedStoreDerivationTransport transport;
  build.target = {WorkerKind::native, node.settings.system.empty() ? native_system() : node.settings.system};
  build.transport = &transport;
  return std::make_unique<PreparedNativeExecution>(*this);
}

auto NativeExecutionService::run(const ExecutionRequest& request) -> ExecutionResult {
  const Node& node = request.node;
  const CardBuild& build = request.build;
  InvocationSummaryData& s = request.summary;
  ExecutionResult result;

  s.environment_id = build.environment.id;
  s.build_cached = build.cached;
  s.emulator = build.emulator;
  s.execution_service = node.settings.execution_service;
  // The environment and its invocation association must survive a crash
  // before the worker can execute any code, including a reused build.
  try {
    request.start([&](sql::Db& db) { History::put_environment(db, s.run_id, build.environment); });
  } catch (...) {
    s.environment_id.clear();
    s.execution_service.clear();
    throw;
  }

  // Cards run as nobody: switched to by root (container), or mapped in
  // bubblewrap's user namespace when tsugumid is not root (shared VM).
  std::optional<uid_t> uid;
  if (::geteuid() == 0 || !std::string(TSUGUMI_BWRAP).empty()) uid = kCardUid;
  WorkerSpawn sp;
  // A package of another system is loaded by the worker its build made for
  // that system, which the kernel runs under the emulator.
  sp.worker_binary = build.worker.empty() ? service_.options().worker_binary.string() : build.worker;
  sp.bash = TSUGUMI_BASH;
  sp.build = build;
  sp.sandbox = node.settings.sandbox;
  sp.uid = uid;
  sp.gid = uid;
  // Project data roots are used by the core to stage inputs, never mounted.
  const std::filesystem::path state_dir = service_.store().state_dir();
  // Another system's binaries run with the emulator the kernel opened when it
  // was registered; Rosetta also reads its own folder while it runs.
  if (const auto& e = build.emulator; !e.empty() && !e.starts_with("/nix/store/"))
    sp.ro_paths.push_back(std::filesystem::path(e).parent_path().string());
  if (!service_.options().control_dir.empty())
    sp.hidden_paths.push_back(service_.options().control_dir.string());
  sp.hidden_paths.push_back(state_dir.string());
  sp.network = node.settings.network;
  const std::string card_key = path_key(node.path);
  sp.card = card_key;
  for (const auto& f : node.settings.host_folders) {
    std::string source = f.path;
    if (const auto& root = service_.options().host_folders; !root.empty())
      source = (root / crypto::sha256_hex(f.path).substr(0, 16)).string();
    std::error_code ec;
    if (!std::filesystem::is_directory(source, ec))
      throw Failed(FailureKind::not_found,
                   "the folder " + f.path + " (/host/" + f.name +
                       ") is not available here: it is missing on this Mac, or the project must be opened again to share it",
                   {{"path", f.path}});
    sp.binds.push_back({source, "/host/" + f.name, f.write});
  }

  // The resident worker of this Card, if it runs the same build under the same conditions.
  std::string key = build.package.value_or("") + "|" + build.bash.value_or("") + "|" + build.pre_run + "|" +
                    build.post_run + "|" + build.views + "|" + (sp.sandbox ? "s" : "u") + "|" + (uid ? std::to_string(*uid) : "-") +
                    "|" + (sp.network ? "n" : "-") + "|" + sp.worker_binary + "|" + build.key +
                    "|" + std::to_string(build.implementation_index);
  for (const auto& b : sp.binds) key += "|" + b.source + ">" + b.target + (b.write ? "w" : "r");
  const bool reuse = node.settings.reuse_worker;
  const auto worker_card = card_key + "|" + std::to_string(build.implementation_index);
  auto lease = workers_.acquire(worker_card, key, sp, reuse);
  WorkerProc& worker = *lease.worker;
  s.worker_reused = lease.reused;
  if (!lease.replaced.empty() && request.log)
    request.log(LogStream::stderr, "Resident worker state started fresh: " + lease.replaced + ".\n");

  auto invocation_log = std::make_shared<workerd::InvocationLog>(
      [sink = request.log](uint64_t stream, const std::string& text) {
        if (sink) sink(static_cast<LogStream>(stream), text);
      }, request.log_limit_bytes);
  // A transfer/staging error does not discard a living resident's state.
  // If the call itself throws, its protocol state is unknown: end the worker.
  bool between_calls = true;
  auto return_worker = [&] {
    if (result.outcome.failure) {
      const auto kind = result.outcome.failure->kind;
      const std::string reason = kind == FailureKind::cancelled ? "the previous call was cancelled" :
                                 kind == FailureKind::timeout ? "the previous call exceeded its deadline" :
                                 worker.alive() ? "the previous call failed" : "the resident worker died";
      workers_.release(worker_card, key, std::move(lease.worker), false, reason);
      return;
    }
    if (worker.alive()) worker.clear_scratch();
    workers_.release(worker_card, key, std::move(lease.worker), reuse);
  };
  try {
    // Folders: without filesystem: true, work/ and tmp/ are the worker's own
    // (local, emptied between calls); only declared File/Directory outputs
    // are copied into the invocation's out/ for downstream Cards/artifacts.
    bool needs_out = false;
    for (const auto& o : node.outputs) needs_out |= o.kind != PortKind::FlatBuffer;
    const std::filesystem::path& dir = request.directory;
    const std::string work = worker.work_dir();
    const std::string tmp = worker.tmp_dir();
    std::string out;
    if (needs_out) {
      std::filesystem::path d = dir / "out";
      std::filesystem::create_directories(d);
      if (::geteuid() == 0 && worker_uid_on_host()) ::chown(d.c_str(), kCardUid, kCardUid);
      out = d.string();
    }

    // One fixed read-only mount per worker preserves reuse across input paths.
    // Copy through the core's no-symlink traversal, not through Card-controlled
    // links or hard links. Only this invocation's File / Directory bytes enter it.
    auto inputs = request.inputs;
    for (size_t i = 0; i < inputs.size(); ++i) {
      if (request.cancel.load()) throw Failed(FailureKind::cancelled, "staging inputs cancelled");
      if (inputs[i].kind == PortKind::FlatBuffer) continue;
      const auto destination = std::filesystem::path(worker.inputs_dir()) / std::to_string(i);
      std::filesystem::create_directories(destination);
      inputs[i] = service_.keep_file_value(inputs[i], destination, request.cancel);
    }

    const std::string recorded_out = out;
    if (!out.empty()) {
      result.out_alias = std::make_pair(worker.out_dir(), out);
      out = worker.out_dir();
    }

    auto ptype = NixRunner::params_type(node);
    std::string params = NixRunner::params_buffer(node);
    std::vector<std::pair<std::string, std::string>> env = {
        {"PATH", TSUGUMI_CARD_PATH},
        {"HOME", work},
        {"LANG", "C.UTF-8"},
        {"TSUGUMI_RUN_ID", s.run_id},
        {"TSUGUMI_CARD", card_key},
        {"TSUGUMI_INVOCATION", std::to_string(s.invocation)},
        {"TSUGUMI_PARAMS_JSON", NixRunner::params_json(node, false)},
        {"TSUGUMI_WORKER", sp.worker_binary},
    };
    std::optional<uint64_t> deadline_ms;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (node.settings.timeout_seconds) {
      deadline_ms = *node.settings.timeout_seconds * 1000;
      deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(*deadline_ms + 500);
    }
    uint64_t call_id = worker.next_call_id();
    std::string frame = worker_call_frame(call_id, s.run_id, node.path, s.invocation, inputs,
                                          ptype->bfbs, ptype->root_type, params, node.outputs, work, tmp, out, env,
                                          deadline_ms);
    // Logs and displays of this call belong to this run and invocation.
    worker.set_logs([invocation_log](LogStream stream, const std::string& text) {
      invocation_log->append(static_cast<uint64_t>(stream), text);
    });
    StagingHandle output_directory(worker.out_dir());
    int64_t t0 = now_ms();
    between_calls = false;
    result.outcome = worker.call(frame, call_id, &request.cancel, deadline, request.display);
    between_calls = true;
    worker.set_logs(nullptr);
    invocation_log->finish();
    request.on_timing(t0, now_ms());
    // Forwarded inputs retain their recorded path, not the worker's temporary
    // staging path. All other output paths go through normal output validation.
    for (auto& [name, bytes] : result.outcome.outputs) {
      const auto* port = node.output(name);
      if (!port || port->kind == PortKind::FlatBuffer || !types::verify(*port->type, bytes)) continue;
      Value value{name, port->kind, port->type, bytes, {}};
      for (size_t i = 0; i < inputs.size(); ++i)
        if (inputs[i].kind == value.kind && path_value(inputs[i]) == path_value(value)) {
          bytes = with_path(value, *path_value(request.inputs[i])).bytes;
          break;
        }
    }

    if (!result.outcome.failure && !recorded_out.empty()) {
      // Only declared outputs cross the worker boundary. Work/tmp and unused
      // output entries are disposable staging, not implicit output trees.
      for (const auto& [name, bytes] : result.outcome.outputs) {
        const auto* port = node.output(name);
        if (!port || port->kind == PortKind::FlatBuffer || !types::verify(*port->type, bytes)) continue;
        const auto path = path_value(Value{name, port->kind, port->type, bytes, {}});
        if (!path) continue;  // normal output validation reports malformed paths
        const std::filesystem::path produced(*path);
        // Forwarded inputs are outside out and were mapped above.
        const auto relative = produced.lexically_relative(worker.out_dir());
        if (relative.empty() || relative.is_absolute() ||
            std::ranges::any_of(relative, [](const auto& part) { return part == ".."; })) continue;
        try {
          fs::copy_staging_output(output_directory.get(), relative,
                                  std::filesystem::path(recorded_out) / relative,
                                  port->kind == PortKind::Directory, request.cancel);
        } catch (const std::exception& error) {
          if (request.cancel.load()) throw Failed(FailureKind::cancelled, "output transfer cancelled");
          throw Failed(FailureKind::output_invalid, error.what());
        }
      }
    }
    if (!result.outcome.failure && request.validate) {
      try {
        request.validate(result);
      } catch (const Failed& error) {
        // Validation failures are failed calls, rather than transfer errors:
        // the next invocation must start without this Card's mutated state.
        result.outcome.failure = error.failure;
      } catch (...) {
        workers_.release(worker_card, key, std::move(lease.worker), false, "the previous call failed validation");
        throw;
      }
    }
    // Dead/cancelled workers are destroyed without inspecting their scratch.
    // A cleanup error must not replace the failure the Card already reported.
    try {
      return_worker();
    } catch (...) {
      if (!result.outcome.failure) throw;
      return result;
    }
  } catch (...) {
    try {
      if (lease.worker) {
        if (between_calls) return_worker();
        else workers_.release(worker_card, key, std::move(lease.worker), false, "the previous call failed");
      }
    } catch (...) {
      // Preserve the original failure if clearing staging also fails.
    }
    try { invocation_log->finish(); } catch (...) {}
    throw;
  }

  return result;
}

}  // namespace tsugumi
