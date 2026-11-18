#include "remote_execution_service.hh"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>

#include "remote_invocation.hh"
#include "service.hh"
#include "store.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

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

auto poll_remote_job(const RemoteJobClient& client, RemoteJobRecord& job)
    -> RemoteJobState {
  const auto& s = job.invocation;
  const auto fingerprint = crypto::sha256_hex(job.spec);
  RemoteJobState reply;
  if (job.cancel_requested) {
    reply = client.cancel(s.execution_service, job.server_id, job.instance,
                          s.execution_job_id, fingerprint);
  } else {
    reply = client.status(s.execution_service, job.server_id, job.instance,
                          s.execution_job_id);
    if (reply.state == "missing" || reply.state == "uploading") {
      if (job.accepted)
        return {"interrupted",
                fingerprint,
                {},
                "the execution service lost a previously accepted job"};
      reply = client.submit(s.execution_service, job.server_id, job.instance,
                            s.execution_job_id, job.spec);
    }
  }
  if (reply.fingerprint != fingerprint)
    return {"interrupted",
            fingerprint,
            {},
            "the execution service returned a different job fingerprint"};
  job.accepted = true;
  return reply;
}

auto remote_failure(const RemoteJobState& result) -> FailureData {
  auto kind = FailureKind::worker_failed;
  if (result.state == "cancelled")
    kind = FailureKind::cancelled;
  else if (result.error.starts_with("timed out after "))
    kind = FailureKind::timeout;
  else if (result.state == "interrupted")
    kind = FailureKind::interrupted;
  return FailureData::make(kind, result.error.empty()
                                     ? "external job " + result.state
                                     : result.error);
}

// An activation whose derivation is exported to `archive` for the target
// `target` (instance `instance`) to build for `system`.
class PreparedRemoteExecution final : public PreparedExecution {
 public:
  PreparedRemoteExecution(Service& service, const RemoteJobClient& client,
                          std::string target, std::string system,
                          std::string server_id, std::string instance,
                          stdfs::path archive)
      : service_(service),
        client_(client),
        target_(std::move(target)),
        system_(std::move(system)),
        server_id_(std::move(server_id)),
        instance_(std::move(instance)),
        archive_(std::move(archive)),
        transport_(archive_) {}
  PreparedRemoteExecution(const PreparedRemoteExecution&) = delete;
  auto operator=(const PreparedRemoteExecution&)
      -> PreparedRemoteExecution& = delete;
  ~PreparedRemoteExecution() override {
    std::error_code ec;
    stdfs::remove(archive_, ec);
  }

  auto transport() const -> const DerivationClosureTransport& {
    return transport_;
  }
  auto system() const -> const std::string& { return system_; }
  auto stage(const std::atomic<bool>& cancel) -> void override {
    if (cancel.load())
      throw Failed(FailureKind::cancelled, "remote staging cancelled");
  }
  auto execute(const ExecutionRequest& request) -> ExecutionResult override;

 private:
  Service& service_;
  const RemoteJobClient& client_;
  std::string target_;
  std::string system_;
  std::string server_id_;
  std::string instance_;
  stdfs::path archive_;
  ArchiveDerivationTransport transport_;
};

auto PreparedRemoteExecution::execute(const ExecutionRequest& request)
    -> ExecutionResult {
  const auto& node = request.node;
  const auto& settings = node.settings;
  auto& s = request.summary;
  stdfs::create_directories(request.directory);
  s.execution_service = target_;
  s.execution_job_id = crypto::sha256_hex(
      service_.project_id() + "\n" + s.run_id + "\n" + path_key(node.path) +
      "\n" + std::to_string(s.invocation));
  const auto archive_digest = stage_remote_archive(
      client_, target_, server_id_, instance_, s.execution_job_id, archive_,
      request.cancel, [&](uint64_t done, uint64_t total) {
        if (request.transfer) request.transfer("upload", "Nix", done, total);
      });
  auto spec = stage_remote_invocation(client_, target_, server_id_, instance_,
                                      s.execution_job_id, service_.file_roots(),
                                      archive_digest, request);
  if (request.cancel.load())
    throw Failed(FailureKind::cancelled,
                 "run cancelled before external submission");

  s.execution_system = system_;
  s.execution_resources = settings.execution_resources;
  s.execution_state = RemoteExecutionState::queued;
  s.worker_reused = false;
  RemoteJobRecord job{{}, instance_, server_id_, std::move(spec)};
  // Cancellation identity is durable before submission; results are never
  // recovered after restart.
  request.start([&](sql::Db& db) {
    job.invocation = s;
    job.cancel_requested = request.cancel_requested();
    History::put_remote_job(db, job);
  });

  auto& history = service_.history();
  ExecutionResult output;
  while (!request.detach.load()) {
    const auto before = s.execution_state;
    if (request.cancel_requested() && !job.cancel_requested) {
      job.cancel_requested = true;
    }
    std::optional<RemoteJobState> reply;
    try {
      reply = poll_remote_job(client_, job);
      s.execution_state = remote_execution_state(reply->state);
      if (request.transfer && !reply->transfer_name.empty())
        request.transfer("copy", reply->transfer_name, reply->transfer_done,
                         reply->transfer_total);
    } catch (const BrokerInstanceChanged& error) {
      s.execution_state = RemoteExecutionState::interrupted;
      output.outcome.failure =
          FailureData::make(FailureKind::interrupted, error.what());
      return output;
    } catch (const BrokerRequestRejected& error) {
      s.execution_state = RemoteExecutionState::failed;
      output.outcome.failure =
          FailureData::make(FailureKind::invalid_request, error.what());
      return output;
    } catch (const std::exception&) {
      reply.reset();
      s.execution_state = RemoteExecutionState::disconnected;
    }
    if (request.detach.load()) throw ExecutionDetached{};
    if (reply && reply->log_truncated && !s.log_truncated_at_bytes) {
      s.log_truncated_at_bytes = request.log_limit_bytes;
      if (request.log_truncated) request.log_truncated();
    }
    if (s.execution_state != before) request.publish();
    if (reply && reply->state == "succeeded") {
      try {
        auto result = import_remote_result(
            client_, target_, server_id_, instance_, s.execution_job_id,
            reply->result_sha256, job.spec, request.directory,
            service_.store().state_dir() / "modules", node.outputs,
            request.display, request.detach,
            [&](std::string_view name, uint64_t done, uint64_t total) {
              if (request.transfer)
                request.transfer("download", name, done, total);
            });
        if (!result.environment.paths.empty()) {
          s.build_cached = !result.built;
          history.write_sync([&](sql::Db& db) {
            s.environment_id =
                History::put_environment(db, s.run_id, result.environment);
          });
        }
        output.log_truncated = result.log_truncated;
        output.logs = std::move(result.logs);
        output.outcome = std::move(result.worker);
        if (output.outcome.failure)
          s.execution_state = RemoteExecutionState::failed;
        return output;
      } catch (const BrokerDisconnected&) {
        s.execution_state = RemoteExecutionState::disconnected;
        request.publish();
      }
    } else if (reply &&
               (reply->state == "failed" || reply->state == "cancelled" ||
                reply->state == "interrupted")) {
      output.outcome.failure = remote_failure(*reply);
      return output;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
  }
  throw ExecutionDetached{};
}

}  // namespace

RemoteExecutionService::RemoteExecutionService(Service& service,
                                               RemoteJobClient& client)
    : service_(service), client_(client) {}

auto RemoteExecutionService::prepare(const Node& node,
                                     const stdfs::path& directory,
                                     BuildRequest& build)
    -> std::unique_ptr<PreparedExecution> {
  const auto& settings = node.settings;
  if (!settings.sandbox || settings.reuse_worker ||
      !settings.host_folders.empty())
    throw Failed(
        FailureKind::unsupported,
        "remote worker requires sandbox, no worker reuse or host folders");
  const auto targets = service_.remote_targets();
  const auto target =
      std::ranges::find(targets, settings.execution_service, &RemoteTarget::id);
  if (target == targets.end())
    throw Failed(FailureKind::unsupported, "execution service " +
                                               settings.execution_service +
                                               " is not registered");
  auto probe = client_.probe(target->id, target->server_id);
  auto system = settings.system.empty() ? probe.native_system : settings.system;
  if (system.empty())
    throw Failed(FailureKind::unsupported, "remote worker system is unknown");
  if (settings.execution_resources && !probe.resource_limits)
    throw Failed(FailureKind::unsupported,
                 "the remote execution daemon cannot enforce resource limits");
  if (std::ranges::find(probe.systems, system) == probe.systems.end())
    throw Failed(
        FailureKind::unsupported,
        "external execution service no longer supports system " + system);
  stdfs::create_directories(directory);
  auto prepared = std::make_unique<PreparedRemoteExecution>(
      service_, client_, target->id, std::move(system),
      std::move(probe.server_id), std::move(probe.instance),
      directory / (".derivation-" + crypto::uuid4()));
  build.target = {WorkerKind::remote, prepared->system()};
  build.transport = &prepared->transport();
  return prepared;
}

auto RemoteExecutionService::release_completed() -> void {
  struct Release {
    std::string job, service, server, instance, fingerprint;
    bool delete_data;
  };
  const auto releases = service_.history().read([](sql::Db& db) {
    std::vector<Release> result;
    auto rows = db.prepare(
        "SELECT job_id,service,server_id,instance,fingerprint,delete_data FROM "
        "remote_releases");
    while (rows.step())
      result.push_back({rows.text(0), rows.text(1), rows.text(2), rows.text(3),
                        rows.text(4), rows.i64(5) != 0});
    return result;
  });
  for (const auto& r : releases) {
    try {
      client_.cancel(r.service, r.server, r.instance, r.job, r.fingerprint);
      // Cancellation relinquishes execution. Only acknowledged, collected data
      // may use the separate payload-deleting release; interruption keeps it.
      if (r.delete_data)
        client_.release(r.service, r.server, r.instance, r.job, r.fingerprint);
    } catch (const BrokerInstanceChanged&) { /* The original journal no longer
                                                exists at this endpoint. */
    } catch (const std::exception&) {
      continue;
    }
    service_.history().write_sync([&](sql::Db& db) {
      db.prepare("DELETE FROM remote_releases WHERE job_id=?")
          .bind(1, r.job)
          .run();
    });
  }
}

auto prepare_remote_artifact(const stdfs::path& run_dir,
                             const stdfs::path& out_dir,
                             const InvocationSummaryData& inv, Value& value)
    -> ArtifactData {
  const auto source = path_value(value);
  if (!source)
    throw Failed(FailureKind::output_invalid, "remote artifact has no path");
  const stdfs::path path(*source);
  const auto relative = path.lexically_relative(out_dir);
  if (relative.empty() || *relative.begin() == ".." ||
      !fs::resolves_inside(out_dir, path))
    throw Failed(FailureKind::output_invalid,
                 "remote artifact is outside the output directory");
  ArtifactData artifact;
  artifact.artifact_id = inv.run_id + "/" + path_key(inv.card) + "/" +
                         std::to_string(inv.invocation) + "/" + value.name;
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
    crypto::Sha256 hash;
    const auto size =
        fs::copy_beneath(out_dir, from, target,
                         [&](std::string_view chunk) { hash.update(chunk); });
    artifact.members.push_back({name, size, hash.finish_hex()});
  };
  if (value.kind == PortKind::File)
    copy(relative, artifact.name);
  else {
    const auto list =
        fs::list_beneath(out_dir, relative == "." ? stdfs::path{} : relative);
    if (!list.links.empty() || !list.special.empty() ||
        list.files.size() > 1000)
      throw Failed(
          FailureKind::output_invalid,
          "remote artifact has symlinks, special files or too many files");
    for (const auto& directory : list.directories)
      stdfs::create_directories(destination / directory);
    for (const auto& file : list.files)
      copy(relative == "." ? stdfs::path(file) : relative / file, file);
  }
  value.artifact_id = artifact.artifact_id;
  return artifact;
}

}  // namespace tsugumi
