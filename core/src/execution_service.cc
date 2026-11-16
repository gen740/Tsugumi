#include "execution_service.hh"

#include <sys/types.h>
#include <unistd.h>

#include <chrono>

#include "history.hh"
#include "run_scratch.hh"
#include "service.hh"
#include "store.hh"
#include "util/crypto.hh"
#include "tsugumi/workerd/worker_execution.hh"
#include "tsugumi/workerd/log.hh"

namespace tsugumi {

namespace {

// The unprivileged identity for Card processes.
constexpr uid_t kCardUid = 65534;

class PreparedNativeExecution final : public PreparedExecution {
 public:
  explicit PreparedNativeExecution(NativeExecutionService& service) : service_(service) {}
  auto execute(const ExecutionRequest& request) -> ExecutionResult override { return service_.run(request); }

 private:
  NativeExecutionService& service_;
};

}  // namespace

NativeExecutionService::NativeExecutionService(Service& service, std::filesystem::path /*scratch_root*/)
    : service_(service), workers_(service.run_scratch() / "workers") {}

auto NativeExecutionService::clear() -> void { workers_.clear(false); }

auto NativeExecutionService::prepare(const Node& node, const std::filesystem::path& /*directory*/,
                                    BuildRequest& build) -> std::unique_ptr<PreparedExecution> {
  static const SharedStoreDerivationTransport transport;
  build.target = {WorkerKind::native, node.settings.system.empty() ? native_system() : node.settings.system};
  build.transport = &transport;
  return std::make_unique<PreparedNativeExecution>(*this);
}

auto NativeExecutionService::run(const ExecutionRequest& request) -> ExecutionResult {
  private_disk_scratch(service_.run_scratch() / "workers");
  const Node& node = request.node;
  const CardBuild& build = request.build;
  InvocationSummaryData& s = request.summary;

  s.build_cached = build.cached;
  s.emulator = build.emulator;
  s.execution_service = node.settings.execution_service;
  // The environment and its invocation association must survive a crash
  // before the worker can execute any code, including a reused build.
  try {
    request.start([&](sql::Db &db) {
      s.environment_id =
          History::put_environment(db, s.run_id, build.environment);
    });
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
  // The shell and PATH are the definition's (the manifest's), so they are in
  // the recorded environment and do not change with this build of the core.
  sp.bash = build.shell;
  sp.program = {build.package, build.bash, build.pre_run, build.post_run, build.views};
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
  s.worker_reused = lease.reused;
  if (!lease.replaced.empty() && request.log)
    request.log(LogStream::standard_error, "Resident worker state started fresh: " + lease.replaced + ".\n");

  auto invocation_log = std::make_shared<workerd::InvocationLog>(
      [sink = request.log](uint64_t stream, const std::string& text) {
        if (sink) sink(static_cast<LogStream>(stream), text);
      }, request.log_limit_bytes);
  workerd::Invocation invocation;
  invocation.run_id = s.run_id;
  invocation.card = node.path;
  invocation.number = s.invocation;
  invocation.inputs = request.inputs;
  invocation.outputs = node.outputs;
  invocation.prepare_params = [&] {
    const auto ptype = NixRunner::params_type(node);
    return workerd::InvocationParameters{ptype->bfbs, ptype->root_type,
        NixRunner::params_buffer(node), NixRunner::params_json(node, false)};
  };
  invocation.path = build.path;
  invocation.worker_binary = sp.worker_binary;
  invocation.output_directory = request.directory / "out";
  if (::geteuid() == 0 && worker_uid_on_host()) invocation.output_owner = std::make_pair(kCardUid, kCardUid);
  invocation.cancel = &request.cancel;
  if (node.settings.timeout_seconds) invocation.timeout_ms = *node.settings.timeout_seconds * 1000;
  invocation.deadline_grace = std::chrono::milliseconds(500);
  invocation.log = [invocation_log](LogStream stream, const std::string& text) {
    invocation_log->append(static_cast<uint64_t>(stream), text);
  };
  invocation.finish_logs = [invocation_log] { invocation_log->finish(); };
  invocation.display = request.display;
  invocation.on_timing = [&](int64_t begin, int64_t end) { request.on_timing(begin, end); };
  // Input storage belongs to the project; only the copied values cross the
  // worker boundary. A fixed read-only mount preserves resident reuse.
  invocation.stage_inputs = [&](WorkerProc& worker) {
    auto inputs = request.inputs;
    for (size_t i = 0; i < inputs.size(); ++i) {
      if (request.cancel.load()) throw Failed(FailureKind::cancelled, "staging inputs cancelled");
      if (inputs[i].kind == PortKind::FlatBuffer) continue;
      const auto destination = std::filesystem::path(worker.inputs_dir()) / std::to_string(i);
      std::filesystem::create_directories(destination);
      inputs[i] = service_.keep_file_value(inputs[i], destination, request.cancel);
    }
    return inputs;
  };
  invocation.validate = request.validate;
  return workerd::run_invocation(workers_, lease, worker_card, key, reuse, invocation);
}

}  // namespace tsugumi
