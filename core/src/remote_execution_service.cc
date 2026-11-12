#include "remote_execution_service.hh"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>

#include "remote_invocation.hh"
#include "service.hh"
#include "store.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi {

namespace stdfs = std::filesystem;

namespace {

auto remote_execution_state(std::string_view state) -> RemoteExecutionState {
  if (state == "queued") return RemoteExecutionState::queued;
  if (state == "running") return RemoteExecutionState::running;
  if (state == "cancelling") return RemoteExecutionState::cancelling;
  if (state == "succeeded") return RemoteExecutionState::succeeded;
  if (state == "failed") return RemoteExecutionState::failed;
  if (state == "cancelled") return RemoteExecutionState::cancelled;
  if (state == "interrupted") return RemoteExecutionState::interrupted;
  throw BrokerDisconnected("execution gateway returned an unknown job state");
}

auto poll_remote_job(const RemoteJobClient& client, RemoteJobRecord& job, bool may_submit) -> RemoteJobState {
  const auto& s = job.invocation;
  const auto fingerprint = crypto::sha256_hex(job.spec);
  RemoteJobState reply;
  if (job.cancel_requested) {
    reply = client.cancel(s.execution_service, job.instance, s.execution_job_id, fingerprint);
  } else {
    reply = client.status(s.execution_service, job.instance, s.execution_job_id);
    if (reply.state == "missing") {
      if (job.accepted) return {"interrupted", fingerprint, {}, "the execution service lost a previously accepted job"};
      if (may_submit) {
        reply = client.submit(s.execution_service, job.instance, s.execution_job_id, job.spec);
      } else {
        reply = client.cancel(s.execution_service, job.instance, s.execution_job_id, fingerprint);
        if (reply.state == "cancelled")
          return {"interrupted", fingerprint, {}, "the external submission was not confirmed before the core restarted"};
      }
    }
  }
  if (reply.fingerprint != fingerprint)
    return {"interrupted", fingerprint, {}, "the execution service returned a different job fingerprint"};
  job.accepted = true;
  return reply;
}

auto remote_failure(const RemoteJobState& result) -> FailureData {
  auto kind = FailureKind::worker_failed;
  if (result.state == "cancelled") kind = FailureKind::cancelled;
  else if (result.state == "interrupted") kind = result.error.find("result timeout") != std::string::npos
                                                      ? FailureKind::timeout : FailureKind::interrupted;
  return FailureData::make(kind, result.error.empty() ? "external job " + result.state : result.error);
}

auto run_dir_of(Service& service, const std::string& run_id) -> stdfs::path {
  return service.store().state_dir() / "runs" / run_id;
}

// An invocation whose derivation is exported to `archive` for the target
// `target` (instance `instance`) to build for `system`.
class PreparedRemoteExecution final : public PreparedExecution {
 public:
  PreparedRemoteExecution(Service& service, const RemoteJobClient& client, std::string target, std::string system,
                          std::string instance, stdfs::path archive)
      : service_(service),
        client_(client),
        target_(std::move(target)),
        system_(std::move(system)),
        instance_(std::move(instance)),
        archive_(std::move(archive)) {}
  PreparedRemoteExecution(const PreparedRemoteExecution&) = delete;
  auto operator=(const PreparedRemoteExecution&) -> PreparedRemoteExecution& = delete;
  ~PreparedRemoteExecution() override {
    std::error_code ec;
    stdfs::remove(archive_, ec);
  }

  auto archive() const -> const stdfs::path& { return archive_; }
  auto system() const -> const std::string& { return system_; }
  auto execute(const ExecutionRequest& request) -> ExecutionResult override;

 private:
  Service& service_;
  const RemoteJobClient& client_;
  std::string target_;
  std::string system_;
  std::string instance_;
  stdfs::path archive_;
};

auto PreparedRemoteExecution::execute(const ExecutionRequest& request) -> ExecutionResult {
  const auto& node = request.node;
  const auto& settings = node.settings;
  auto& s = request.summary;
  auto spec = stage_remote_invocation(client_, target_, instance_, service_.project_id(), service_.store().state_dir(),
                                      archive_, request);
  if (request.cancel.load()) throw Failed(FailureKind::cancelled, "run cancelled before external submission");

  s.execution_service = target_;
  s.execution_job_id = crypto::sha256_hex(service_.project_id() + "\n" + s.run_id + "\n" + path_key(node.path) + "\n" +
                                          std::to_string(s.invocation));
  s.execution_system = system_;
  s.execution_resources = settings.execution_resources;
  s.execution_state = RemoteExecutionState::queued;
  s.worker_reused = false;
  RemoteJobRecord job{{}, instance_, std::move(spec), remote_recovery_contract(node, request.inputs, request.sources),
                      request.directory.lexically_relative(run_dir_of(service_, s.run_id)).generic_string()};
  // The job is durable before it can be submitted.
  request.start([&](sql::Db& db) {
    job.invocation = s;
    // RunCancel sets the flag before its synchronous database update. An
    // insertion ordered after that update must carry the same intent.
    job.cancel_requested = request.cancel_requested.load();
    History::put_remote_job(db, job);
  });

  auto& history = service_.history();
  ExecutionResult output;
  while (!request.detach.load()) {
    const auto before = s.execution_state;
    const bool accepted = job.accepted;
    if (request.cancel_requested.load() && !job.cancel_requested) {
      job.cancel_requested = true;
      history.write_sync([&](sql::Db& db) { History::put_remote_job(db, job); });
    }
    std::optional<RemoteJobState> reply;
    try {
      reply = poll_remote_job(client_, job, true);
      s.execution_state = remote_execution_state(reply->state);
    } catch (const BrokerInstanceChanged& error) {
      s.execution_state = RemoteExecutionState::interrupted;
      output.outcome.failure = FailureData::make(FailureKind::interrupted, error.what());
      return output;
    } catch (const BrokerRequestRejected& error) {
      s.execution_state = RemoteExecutionState::failed;
      output.outcome.failure = FailureData::make(FailureKind::invalid_request, error.what());
      return output;
    } catch (const std::exception&) {
      reply.reset();
      s.execution_state = RemoteExecutionState::disconnected;
    }
    if (request.detach.load()) throw ExecutionDetached{};
    if (s.execution_state != before || job.accepted != accepted) {
      job.invocation = s;
      history.write_sync([&](sql::Db& db) { History::put_remote_job(db, job); });
      request.publish();
    }
    if (reply && reply->state == "succeeded") {
      output.logs.clear();
      try {
        auto result = import_remote_result(client_, target_, instance_, reply->result_sha256, job.spec, request.directory,
            node.outputs, [&](LogStream stream, const std::string& line) { output.logs.emplace_back(stream, line); },
            request.display, request.detach);
        if (!result.environment.id.empty()) {
          s.environment_id = result.environment.id;
          s.build_cached = !result.built;
          history.write_sync([&](sql::Db& db) { History::put_environment(db, s.run_id, result.environment); });
        }
        output.outcome = std::move(result.worker);
        if (output.outcome.failure) s.execution_state = RemoteExecutionState::failed;
        return output;
      } catch (const BrokerDisconnected&) {
        s.execution_state = RemoteExecutionState::disconnected;
        request.publish();
      }
    } else if (reply && (reply->state == "failed" || reply->state == "cancelled" || reply->state == "interrupted")) {
      output.outcome.failure = remote_failure(*reply);
      return output;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
  }
  throw ExecutionDetached{};
}

}  // namespace

RemoteExecutionService::RemoteExecutionService(Service& service, RemoteJobClient& client)
    : service_(service), client_(client) {}

auto RemoteExecutionService::prepare(const Node& node, const stdfs::path& directory, BuildRequest& build)
    -> std::unique_ptr<PreparedExecution> {
  const auto& settings = node.settings;
  const auto targets = service_.remote_targets();
  if (auto diagnostic = Service::execution_diagnostic(node, targets))
    throw Failed(FailureKind::unsupported, diagnostic->message);
  const auto target = std::ranges::find(targets, settings.execution_service, &RemoteTarget::id);
  auto system = settings.system.empty() ? target->native_system : settings.system;
  auto probe = client_.probe(target->id);
  if (std::ranges::find(probe.systems, system) == probe.systems.end())
    throw Failed(FailureKind::unsupported, "external execution service no longer supports system " + system);
  stdfs::create_directories(directory);
  auto prepared = std::make_unique<PreparedRemoteExecution>(service_, client_, target->id, std::move(system),
                                                            std::move(probe.instance),
                                                            directory / (".derivation-" + crypto::uuid4()));
  build.remote_system = prepared->system();
  build.derivation_archive = prepared->archive();
  return prepared;
}

auto RemoteExecutionService::poll_recovered(RemoteJobRecord& job) -> std::optional<RemoteJobState> {
  auto& inv = job.invocation;
  std::optional<RemoteJobState> reply;
  try {
    reply = poll_remote_job(client_, job, false);
    inv.execution_state = remote_execution_state(reply->state);
  } catch (const BrokerInstanceChanged& error) {
    inv.execution_state = RemoteExecutionState::interrupted;
    reply = RemoteJobState{"interrupted", {}, {}, error.what()};
  } catch (const BrokerRequestRejected& error) {
    inv.execution_state = RemoteExecutionState::failed;
    reply = RemoteJobState{"failed", {}, {}, error.what()};
  } catch (const std::exception&) {
    reply.reset();
    inv.execution_state = RemoteExecutionState::disconnected;
  }
  return reply;
}

auto RemoteExecutionService::recover(RemoteJobRecord& job, const RemoteJobState& reply, const std::atomic<bool>& stop)
    -> RecoveredRemoteJob {
  auto& inv = job.invocation;
  RecoveredRemoteJob recovered;
  if (reply.state != "succeeded") {
    inv.failure = remote_failure(reply);
    return recovered;
  }
  auto& result = recovered.record;
  try {
    auto contract = remote_recovery_contract(job.contract);
    const auto run_dir = run_dir_of(service_, inv.run_id);
    const auto directory = run_dir / remote::relative_path(job.directory);
    auto imported = import_remote_result(client_, inv.execution_service, job.instance, reply.result_sha256,
        job.spec, directory, contract.outputs,
        [&](LogStream stream, const std::string& line) { recovered.logs.emplace_back(stream, line); },
        [&](DisplayData display) {
          if (display.run_id == inv.run_id && display.invocation == inv.invocation && !display.validate()) {
            display.card = inv.card;
            result.display = std::move(display);
          }
        }, stop);
    recovered.environment = std::move(imported.environment);
    if (!recovered.environment.id.empty()) {
      inv.environment_id = recovered.environment.id;
      inv.build_cached = !imported.built;
    }
    inv.failure = imported.worker.failure;
    result.record_io = contract.record_io;
    result.inputs = std::move(contract.inputs);
    result.input_sources = std::move(contract.sources);
    if (!inv.failure) {
      for (auto& [name, bytes] : imported.worker.outputs) {
        const auto port = std::ranges::find(contract.outputs, name, &types::PortType::name);
        Value value{name, port->kind, port->type, std::move(bytes), {}};
        if (!contract.artifact_output.empty() && name == contract.artifact_output)
          recovered.artifact = prepare_remote_artifact(run_dir, directory / "out", inv, value);
        if (result.record_io) result.outputs.push_back(std::move(value));
      }
    }
  } catch (const BrokerDisconnected&) {
    throw;
  } catch (const Failed& error) {
    inv.failure = error.failure;
  } catch (const std::exception& error) {
    inv.failure = FailureData::make(FailureKind::output_invalid, error.what());
  }
  if (inv.failure) inv.execution_state = RemoteExecutionState::failed;
  return recovered;
}

auto prepare_remote_artifact(const stdfs::path& run_dir, const stdfs::path& out_dir,
                             const InvocationSummaryData& inv, Value& value) -> ArtifactData {
  const auto source = path_value(value);
  if (!source) throw Failed(FailureKind::output_invalid, "recovered artifact has no path");
  const stdfs::path path(*source);
  const auto relative = path.lexically_relative(out_dir);
  if (relative.empty() || *relative.begin() == ".." || !fs::resolves_inside(out_dir, path))
    throw Failed(FailureKind::output_invalid, "recovered artifact is outside the output directory");
  const auto& id = inv.execution_job_id;
  ArtifactData artifact;
  artifact.artifact_id = id.substr(0, 8) + "-" + id.substr(8, 4) + "-" + id.substr(12, 4) + "-" +
                         id.substr(16, 4) + "-" + id.substr(20, 12);
  artifact.run_id = inv.run_id;
  artifact.card = inv.card;
  artifact.invocation = inv.invocation;
  artifact.output = value.name;
  artifact.kind = value.kind;
  artifact.name = path.filename().string();
  const auto destination = run_dir / "artifacts" / artifact.artifact_id;
  stdfs::remove_all(destination);
  stdfs::create_directories(destination);
  const auto copy = [&](const stdfs::path& from, const std::string& name) {
    const auto target = destination / remote::relative_path(name);
    stdfs::create_directories(target.parent_path());
    std::ofstream file(target, std::ios::binary);
    if (!file) throw std::runtime_error("cannot stage recovered artifact");
    crypto::Sha256 hash;
    const auto size = fs::stream_beneath(out_dir, from, uint64_t{64} << 30, [&](std::string_view chunk) {
      file.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
      if (!file) throw std::runtime_error("cannot stage recovered artifact");
      hash.update(chunk);
    });
    file.close();
    if (!file) throw std::runtime_error("cannot store recovered artifact");
    artifact.members.push_back({name, size, hash.finish_hex()});
  };
  if (value.kind == PortKind::File) copy(relative, artifact.name);
  else {
    const auto list = fs::list_beneath(out_dir, relative == "." ? stdfs::path{} : relative);
    if (!list.links.empty() || list.files.size() > 1000)
      throw Failed(FailureKind::output_invalid, "recovered artifact has symlinks or too many files");
    for (const auto& file : list.files) copy(relative == "." ? stdfs::path(file) : relative / file, file);
  }
  value.artifact_id = artifact.artifact_id;
  return artifact;
}

}  // namespace tsugumi
