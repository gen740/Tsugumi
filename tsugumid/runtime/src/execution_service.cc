#include "execution_service.hh"

#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <mutex>

#include "history.hh"
#include "run_scratch.hh"
#include "service.hh"
#include "store.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/log.hh"
#include "tsugumi/util/system.hh"
#include "tsugumi/workerd/protocol.hh"
#include "tsugumi/workerd/store.hh"
#include "tsugumi/workerd/worker_execution.hh"

namespace tsugumi {

namespace {

// The unprivileged identity for Card processes.
#ifdef __linux__
constexpr uid_t kCardUid = 65534;
#endif

class PreparedLocalExecution final : public PreparedExecution {
 public:
  explicit PreparedLocalExecution(LocalExecutionService& service)
      : service_(service) {}
  auto execute(const ExecutionRequest& request) -> ExecutionResult override {
    return service_.run(request);
  }

 private:
  LocalExecutionService& service_;
};

}  // namespace

LocalExecutionService::LocalExecutionService(
    Service& service, std::filesystem::path /*scratch_root*/)
    : service_(service),
      workers_(service.options().workerd_binary.empty()
                   ? service.options().worker_binary.parent_path() /
                         "tsugumi-workerd"
                   : service.options().workerd_binary,
               service.run_scratch() / "workers"),
      secrets_salt_(crypto::uuid4()) {}

auto LocalExecutionService::clear() -> void {
  workers_.clear();
  std::lock_guard lock(secrets_mutex_);
  secret_versions_.clear();
}

auto LocalExecutionService::invalidate(const std::string& card,
                                       const std::string& reason) -> void {
  try {
    workers_.invalidate(card, reason);
  } catch (const Failed& error) {
    log::warn("cannot retire Card worker " + card + ": " + error.what());
  }
}

auto LocalExecutionService::prepare(const Node& node,
                                    const std::filesystem::path& /*directory*/,
                                    BuildRequest& build)
    -> std::unique_ptr<PreparedExecution> {
  if (node.settings.sandbox.limits.limited() && !worker_limits_available())
    throw Failed(FailureKind::unsupported,
                 "sandbox.limits requires a delegated cgroup v2 subtree");
  if (node.settings.grants.gui &&
      (!service_.options().gcroots_dir.empty() || !worker_gui_available()))
    throw Failed(FailureKind::unsupported,
                 "grants.gui requires a native Linux worker with an available "
                 "GUI socket");
  static const SharedStoreDerivationTransport transport;
  build.target = {WorkerKind::native,
                  build.probe_only
                      ? native_system()
                      : run_system(node.settings, native_system())};
  build.transport = &transport;
  return std::make_unique<PreparedLocalExecution>(*this);
}

auto LocalExecutionService::run(const ExecutionRequest& request)
    -> ExecutionResult {
  private_disk_scratch(service_.run_scratch() / "workers");
  const Node& node = request.node;
  const CardBuild& build = request.build;
  InvocationSummaryData& s = request.summary;

  // Only a salted digest participates in residency. The values are sent
  // through the local launch channel when the process is first started.
  const auto card_key =
      path_key(node.path) + (build.card_probe ? "|probe" : "");
  auto secrets = std::make_shared<WorkerSecrets>();
  try {
    secrets->values = service_.card_secrets(node, request.stop);
  } catch (const Failed& error) {
    if (error.failure.kind == FailureKind::not_found)
      invalidate(card_key, "a Card secret is no longer available");
    throw;
  }
  std::string secret_version;
  if (node.settings.reuse_worker && !secrets->values.empty()) {
    crypto::Sha256 digest;
    digest.update(secrets_salt_);
    for (const auto& [name, value] : secrets->values) {
      const uint64_t name_size = name.size();
      digest.update(&name_size, sizeof name_size);
      digest.update(name);
      const uint64_t size = value.size();
      digest.update(&size, sizeof size);
      digest.update(value);
    }
    secret_version = digest.finish_hex();
  }
  {
    bool changed = false;
    {
      std::lock_guard lock(secrets_mutex_);
      if (auto prior = secret_versions_.find(card_key);
          prior != secret_versions_.end()) {
        changed = prior->second != secret_version;
        if (secret_version.empty())
          secret_versions_.erase(prior);
        else
          prior->second = secret_version;
      } else if (!secret_version.empty()) {
        secret_versions_.emplace(card_key, secret_version);
      }
    }
    if (changed) invalidate(card_key, "a Card secret changed");
  }
  s.secrets.clear();
  for (const auto& secret : secrets->values) s.secrets.push_back(secret.first);

  s.build_cached = build.cached;
  s.build_substituter = build.substituter;
  s.build_pushed = build.pushed;
  s.emulator = build.emulator;
  s.execution_service = kLocalExecutionService;
  // The environment and its invocation association must survive a crash
  // before the worker can execute any code, including a reused build.
  try {
    request.start([&](sql::Db& db) {
      s.environment_id =
          History::put_environment(db, s.run_id, build.environment);
    });
  } catch (...) {
    s.environment_id.clear();
    s.execution_service.clear();
    throw;
  }

  WorkerSpawn sp;
#ifdef __linux__
  // Linux identity isolation uses a dedicated uid in bubblewrap.
  if (node.settings.sandbox.identity &&
      (::geteuid() == 0 || worker_has_bwrap()))
    sp.uid = sp.gid = kCardUid;
#endif
  // A package of another system is loaded by the worker its build made for
  // that system, which the kernel runs under the emulator.
  sp.worker_binary = build.worker.empty()
                         ? service_.options().worker_binary.string()
                         : build.worker;
  if (!secrets->values.empty()) sp.secrets = std::move(secrets);
  sp.package = build.package;
  sp.card_probe = build.card_probe;
  sp.packed = service_.packed_builds() != nullptr;
  sp.packed_debug = service_.packed_debug();
  sp.sandbox = node.settings.sandbox;
  sp.shared_vm = !service_.options().gcroots_dir.empty();
  sp.closure = build.environment.paths;
#ifdef __linux__
  if (sp.sandbox.filesystem && sp.worker_binary.starts_with("/nix/store/")) {
    const auto end =
        sp.worker_binary.find('/', std::string_view("/nix/store/").size());
    const auto root = sp.worker_binary.substr(0, end);
    if (std::ranges::find(sp.closure, root) == sp.closure.end()) {
      std::unique_lock lock(closure_mutex_);
      auto found = worker_closures_.find(root);
      if (found == worker_closures_.end()) {
        lock.unlock();
        auto closure = NixStore::closure(root);
        lock.lock();
        found = worker_closures_.try_emplace(root, std::move(closure)).first;
      }
      sp.closure.insert(sp.closure.end(), found->second.begin(),
                        found->second.end());
    }
  }
#endif
  const std::filesystem::path state_dir = service_.store().state_dir();
  // Another system's binaries run with the emulator the kernel opened when it
  // was registered; Rosetta also reads its own folder while it runs.
  if (const auto& e = build.emulator;
      !e.empty() && !e.starts_with("/nix/store/")) {
    sp.ro_paths.push_back(std::filesystem::path(e).parent_path().string());
    constexpr auto socket = "/run/rosettad/rosetta.sock";
    if (e == "/run/rosetta/rosetta" && std::filesystem::is_socket(socket))
      sp.ro_paths.push_back(socket);
  }
  if (sp.shared_vm && !service_.options().control_dir.empty())
    sp.hidden_paths.push_back(service_.options().control_dir.string());
  if (sp.shared_vm) sp.hidden_paths.push_back(state_dir.string());
  sp.network = node.settings.grants.network;
  sp.gui = node.settings.grants.gui;
  sp.binds = service_.host_path_binds(node);

  std::filesystem::path destination;
  if (!build.card_probe) {
    destination = service_.artifact_destination(s);
    sp.artifact_store = (state_dir / "artifacts").string();
    sp.artifact_staging = state_dir / "artifact-invocations";
    std::filesystem::create_directories(sp.artifact_store);
  }

  // The resident worker of this Card, if it runs the same build under the same
  // conditions.
  std::string key =
      build.package + "|" + std::to_string(sp.sandbox.filesystem) +
      std::to_string(sp.sandbox.pid) + std::to_string(sp.sandbox.ipc) +
      std::to_string(sp.sandbox.identity) + "|" +
      std::to_string(sp.sandbox.limits.cpu_millis) + "|" +
      std::to_string(sp.sandbox.limits.memory_bytes) + "|" +
      std::to_string(sp.gui) + "|" + (sp.uid ? std::to_string(*sp.uid) : "-") +
      "|" + (sp.network ? "n" : "-") + "|" + sp.worker_binary + "|" +
      build.key + "|" + std::to_string(build.implementation_index) + "|" +
      secret_version;
  for (const auto& b : sp.binds)
    key += "|" + b.source + ">" + b.target + (b.write ? "w" : "r");
  const bool reuse = node.settings.reuse_worker;
  const auto worker_card =
      card_key + "|" + std::to_string(build.implementation_index);
  auto lease = workers_.acquire(worker_card, key, sp, reuse);
  sp.secrets.reset();
  s.worker_reused = lease.reused;
  if (!lease.replaced.empty() && request.log)
    request.log(
        LogStream::output,
        "Resident worker state started fresh: " + lease.replaced + ".\n");

  workerd::Invocation invocation;
  invocation.inputs = request.inputs;
  invocation.outputs = node.outputs;
  invocation.artifact_directory = destination;
  invocation.artifact_sequence = s.artifact_sequence;
  invocation.resolve_artifact = [this](const ArtifactKey& id, std::stop_token) {
    return service_.resolve_artifact(id);
  };
  invocation.output_budget = {node.settings.output_limit_bytes,
                              node.settings.output_limit_entries};
  invocation.cancel = request.stop;
  if (node.settings.timeout_seconds)
    invocation.timeout_ms = *node.settings.timeout_seconds * 1000;
  invocation.deadline_grace = std::chrono::milliseconds(500);
  invocation.log = request.log;
  invocation.display = request.display;
  invocation.view_control =
      [service = &service_, origin = Service::ViewOrigin(s),
       views = build.views, endpoint = lease.worker->view_receiver()](
          const WorkerViewRequest& control) {
        return service->view_control(origin, views, endpoint, control);
      };
  invocation.on_timing = [&](int64_t begin, int64_t end) {
    request.on_timing(begin, end);
  };
  invocation.validate = request.validate;
  auto completed = workerd::run_invocation(
      [&](auto worker, bool keep, const auto& reason) {
        workers_.release(std::move(worker), keep, reason);
      },
      lease, reuse, invocation);
  if (!build.card_probe) service_.record_artifacts(s, completed.created);
  return {std::move(completed.outputs), std::move(completed.failure)};
}

}  // namespace tsugumi
