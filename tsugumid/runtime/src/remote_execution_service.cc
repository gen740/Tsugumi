#include "remote_execution_service.hh"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <optional>

#include "remote_invocation.hh"
#include "tsugumi/workerd/invocation.hh"
#include "service.hh"
#include "store.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/workerd/protocol.hh"

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

// Records a remote job's log while it runs. The cursor counts the bytes
// received, so reconnects and listener restarts within one core life continue
// where they stopped and never repeat an entry. A trailing partial entry waits
// for the rest of its bytes.
class LiveRemoteLog {
 public:
  LiveRemoteLog(uint64_t limit, WorkerProc::LogSink sink)
      : limit_(limit), sink_(std::move(sink)) {}

  auto cursor() const -> uint64_t { return received_; }

  // Records the complete entries of a reply's slice. Returns whether the
  // slice advanced the cursor and the job's log holds more bytes.
  auto accept(const RemoteJobState& reply) -> bool {
    if (closed_ || reply.log_offset != received_ ||
        reply.log_size < received_ || reply.log.size() > limit_ - received_)
      return false;
    if (reply.log.empty()) return false;
    received_ += reply.log.size();
    pending_ += reply.log;
    size_t used = 0;
    try {
      used = remote::decode_log_entries(
          pending_, [&](LogStream kind, std::string text, std::string_view raw) {
            hash_.update(raw.data(), raw.size());
            recorded_ += raw.size();
            if (sink_) sink_(kind, text);
          });
    } catch (const std::exception&) {
      // The completed result's log decides; its import reports the error.
      closed_ = true;
      return false;
    }
    pending_.erase(0, used);
    return reply.log_size > received_;
  }

  // The recorded prefix for the completed result's import. Collection ends
  // here: the result carries every later entry.
  auto recorded() -> const RemoteLogPrefix& {
    if (!prefix_) {
      prefix_ = {recorded_, recorded_ ? hash_.finish_hex() : std::string{}};
      closed_ = true;
    }
    return *prefix_;
  }

 private:
  uint64_t limit_;
  WorkerProc::LogSink sink_;
  uint64_t received_ = 0, recorded_ = 0;
  std::string pending_;
  crypto::Sha256 hash_;
  std::optional<RemoteLogPrefix> prefix_;
  bool closed_ = false;
};

auto poll_remote_job(const RemoteJobClient& client, RemoteJobRecord& job,
                     uint64_t log_offset, std::stop_token stop)
    -> RemoteJobState {
  const auto& s = job.invocation;
  const auto fingerprint = crypto::sha256_hex(job.spec);
  RemoteJobState reply;
  if (job.cancel_requested) {
    reply = client.cancel(s.execution_service, job.instance, s.execution_job_id,
                          fingerprint, log_offset);
  } else {
    reply = client.status(s.execution_service, job.instance, s.execution_job_id,
                          log_offset, stop);
    if (reply.state == "missing" || reply.state == "uploading") {
      if (job.accepted)
        return {"interrupted",
                fingerprint,
                {},
                "the execution service lost a previously accepted job"};
      reply = client.submit(s.execution_service, job.instance,
                            s.execution_job_id, job.spec, stop);
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
                          std::string instance, stdfs::path archive)
      : service_(service),
        client_(client),
        target_(std::move(target)),
        system_(std::move(system)),
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
  // A Card secret that is not set fails here, before anything moves to the
  // worker. The values are not kept: the worker asks for them once only the
  // Card's process remains to start.
  if (!settings.secrets.empty()) {
    auto check = service_.card_secrets(node, request.stop);
    for (auto& secret : check) workerd::clear_secret(secret.second);
  }
  s.execution_service = target_;
  s.execution_job_id = crypto::sha256_hex(
      service_.project_id() + "\n" + s.run_id + "\n" + path_key(node.path) +
      "\n" + std::to_string(s.invocation));
  const auto archive_digest = stage_remote_archive(
      client_, target_, instance_, s.execution_job_id, archive_, request.stop,
      [&](uint64_t done, uint64_t total) {
        if (request.transfer) request.transfer("upload", "Nix", done, total);
      });
  auto spec =
      stage_remote_invocation(client_, target_, instance_, s.execution_job_id,
                              service_.artifact_grants(request.inputs), archive_digest, request);
  if (request.stop.stop_requested())
    throw Failed(FailureKind::cancelled,
                 "run cancelled before external submission");

  s.execution_system = system_;
  if (settings.sandbox.limits.limited())
    s.execution_resources = settings.sandbox.limits;
  s.execution_state = RemoteExecutionState::queued;
  s.worker_reused = false;
  RemoteJobRecord job{{}, instance_, std::move(spec)};
  // Cancellation identity is durable before submission; results are never
  // recovered after restart.
  request.start([&](sql::Db& db) {
    job.invocation = s;
    job.cancel_requested = request.cancel_requested();
    History::put_remote_job(db, job);
  });

  auto receiver = std::make_shared<WorkerProc::ViewReceiver>();
  receiver->remote = [&client = client_, target = target_, instance = instance_,
                      id = s.execution_job_id, stop = request.stop](const auto& message) {
    try {
      client.send_view(target, instance, id, message, stop);
      return true;
    } catch (const std::exception&) {
      return false;
    }
  };
  service_.set_view_receiver(node.path, receiver);
  // Close the route and drain any send using it before execute returns.
  struct CloseReceiver {
    WorkerProc::ViewReceiver& receiver;
    ~CloseReceiver() {
      std::lock_guard lock(receiver.mutex);
      receiver.remote = nullptr;
    }
  } close_receiver{*receiver};
  auto& history = service_.history();
  ExecutionResult output;
  LiveRemoteLog live_log(kRemoteLogTransportBytes, request.log);
  bool secrets_sent = false;
  std::mutex wait_mutex;
  std::condition_variable_any next_status;
  while (!request.detach.stop_requested()) {
    const auto before = s.execution_state;
    job.cancel_requested |= request.cancel_requested();
    const auto stop = job.cancel_requested ? request.detach : request.stop;
    std::optional<RemoteJobState> reply;
    bool more_log = false;
    try {
      reply = poll_remote_job(client_, job, live_log.cursor(), stop);
      if (stop.stop_requested()) continue;
      more_log = live_log.accept(*reply);
      if (request.display)
        for (const auto& display : reply->displays) request.display(display, request.stop);
      s.execution_state = remote_execution_state(reply->state);
      if (request.transfer && !reply->transfer_name.empty())
        request.transfer("copy", reply->transfer_name, reply->transfer_done,
                         reply->transfer_total);
    } catch (const BrokerInstanceChanged& error) {
      s.execution_state = RemoteExecutionState::interrupted;
      output.failure =
          FailureData::make(FailureKind::interrupted, error.what());
      return output;
    } catch (const BrokerRequestRejected& error) {
      s.execution_state = RemoteExecutionState::failed;
      output.failure =
          FailureData::make(FailureKind::invalid_request, error.what());
      return output;
    } catch (const std::exception&) {
      if (stop.stop_requested()) continue;
      reply.reset();
      s.execution_state = RemoteExecutionState::disconnected;
    }
    if (request.detach.stop_requested()) throw ExecutionDetached{};
    // The job has built its environment: its Card secrets go to the worker
    // for this job only, once. A refused or unconfirmed delivery is never
    // repeated: the job then fails on the worker, which reports why (after a
    // listener restart, its invocation has already lost them).
    if (reply && reply->secrets_wanted && !secrets_sent &&
        !job.cancel_requested) {
      secrets_sent = true;
      try {
        auto secrets = service_.card_secrets(node, request.stop);
        s.secrets.clear();
        for (const auto& secret : secrets) s.secrets.push_back(secret.first);
        client_.deliver_secrets(
            s.execution_service, job.instance, s.execution_job_id,
            crypto::sha256_hex(job.spec), secrets, request.stop);
      } catch (const Failed& error) {
        s.execution_state = RemoteExecutionState::failed;
        output.failure = error.failure;
        return output;
      } catch (const std::exception&) {
      }
    }
    if (s.execution_state != before) request.publish();
    if (reply && reply->state == "succeeded") {
      // A stopped job keeps its workspace until release: collect the rest
      // of its log first. Each poll is a bounded exchange that advances the
      // cursor and stays interruptible as before.
      if (reply->display_pending) continue;
      if (job.cancel_requested) {
        if (more_log) continue;
        output.failure =
            FailureData::make(FailureKind::cancelled, "Run cancelled");
        return output;
      }
      try {
        auto result = import_remote_result(
            client_, target_, instance_, s.execution_job_id,
            reply->result_sha256, job.spec, request.directory,
            node.outputs, request.inputs, live_log.recorded(), request.log,
            request.stop,
            [&](std::string_view name, uint64_t done, uint64_t total) {
              if (request.transfer)
                request.transfer("download", name, done, total);
            },
            request.on_remote_validation);
        if (!result.environment.paths.empty()) {
          s.build_cached = !result.built;
          history.write_sync([&](sql::Db& db) {
            s.environment_id =
                History::put_environment(db, s.run_id, result.environment);
          });
        }
        service_.seal_artifacts(s, request.directory / "artifacts", result.created);
        output.outputs = std::move(result.outputs);
        output.auxiliary_logs = std::move(result.auxiliary_logs);
        output.auxiliary_logs_truncated = result.auxiliary_logs_truncated;
        output.failure = std::move(result.failure);
        if (output.failure) s.execution_state = RemoteExecutionState::failed;
        return output;
      } catch (const BrokerDisconnected&) {
        if (request.stop.stop_requested()) continue;
        s.execution_state = RemoteExecutionState::disconnected;
        request.publish();
      }
    } else if (reply &&
               (reply->state == "failed" || reply->state == "cancelled" ||
                reply->state == "interrupted")) {
      if (more_log) continue;
      output.failure = remote_failure(*reply);
      return output;
    }
    // A log behind the job's own continues without waiting.
    if (more_log || (reply && reply->display_pending)) continue;
    // The worker protocol reports progress by polling. Stop interrupts the
    // interval and the exchange so Cancel can use a new connection immediately.
    std::unique_lock lock(wait_mutex);
    next_status.wait_for(lock, stop, std::chrono::milliseconds(750),
                         [] { return false; });
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
  if (!settings.sandbox.filesystem || !settings.sandbox.pid ||
      settings.reuse_worker || settings.grants.gui ||
      !settings.grants.host_paths.empty())
    throw Failed(
        FailureKind::unsupported,
        "remote workers require filesystem and PID isolation and do not "
        "support worker reuse, GUI or host path grants");
  const auto targets = service_.remote_targets(build.cancel);
  if (build.cancel.stop_requested())
    throw Failed(FailureKind::cancelled, "worker selection cancelled");
  const auto target =
      std::ranges::find(targets, settings.execution_service, &RemoteTarget::id);
  if (target == targets.end())
    throw Failed(FailureKind::unsupported, "execution service " +
                                               settings.execution_service +
                                               " is not registered");
  auto probe = client_.probe(target->id, target->server_id, build.cancel);
  auto system = settings.system.empty() ? probe.native_system : settings.system;
  if (settings.sandbox.limits.limited() && !probe.resource_limits)
    throw Failed(FailureKind::unsupported,
                 "the remote execution daemon cannot enforce resource limits");
  if (!settings.system.empty() &&
      std::ranges::find(probe.systems, system) == probe.systems.end())
    throw Failed(
        FailureKind::unsupported,
        "external execution service no longer supports system " + system);
  stdfs::create_directories(directory);
  auto prepared = std::make_unique<PreparedRemoteExecution>(
      service_, client_, target->id, std::move(system),
      std::move(probe.instance),
      directory / (".derivation-" + crypto::uuid4()));
  build.target = {WorkerKind::remote, prepared->system()};
  build.transport = &prepared->transport();
  return prepared;
}

auto RemoteExecutionService::release_completed() -> void {
  struct Release {
    std::string job, service, instance, fingerprint;
    bool delete_data;
  };
  const auto releases = service_.history().read([](sql::Db& db) {
    std::vector<Release> result;
    auto rows = db.prepare(
        "SELECT job_id,service,instance,fingerprint,delete_data FROM "
        "remote_releases");
    while (rows.step())
      result.push_back({rows.text(0), rows.text(1), rows.text(2), rows.text(3),
                        rows.i64(4) != 0});
    return result;
  });
  for (const auto& r : releases) {
    try {
      client_.cancel(r.service, r.instance, r.job, r.fingerprint);
      // Cancellation relinquishes execution. Only acknowledged, collected data
      // may use the separate payload-deleting release; interruption keeps it.
      if (r.delete_data)
        client_.release(r.service, r.instance, r.job, r.fingerprint);
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


}  // namespace tsugumi
