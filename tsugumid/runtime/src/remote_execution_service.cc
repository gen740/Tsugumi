#include "remote_execution_service.hh"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

#include "remote_invocation.hh"
#include "service.hh"
#include "store.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/log.hh"
#include "tsugumi/workerd/artifact_id.hh"
#include "tsugumi/workerd/cache_publish.hh"
#include "tsugumi/workerd/cache_relay_store.hh"
#include "tsugumi/workerd/cache_transfer.hh"
#include "tsugumi/workerd/frame.hh"
#include "tsugumi/workerd/invocation.hh"
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

auto execution_activity(const RemoteJobState& reply) -> ExecutionActivity {
  using A = Tsugumi::RemoteWorker::JobActivity;
  if (reply.state != "running") return ExecutionActivity::unspecified;
  switch (reply.activity) {
    case A::StoreWait:
      return ExecutionActivity::waiting_store;
    case A::Preparing:
      return ExecutionActivity::preparing;
    case A::Building:
      return ExecutionActivity::building;
    case A::Substituting:
      return ExecutionActivity::substituting;
    case A::Card:
      return ExecutionActivity::running;
    case A::Pushing:
      return ExecutionActivity::pushing;
    default:
      return ExecutionActivity::unspecified;
  }
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

// Each route has a separate reverse window and writer, so broker I/O for
// a stalled route cannot delay another route or the job's status polls.
class RemoteViewInput {
 public:
  using Send =
      std::function<void(const WorkerViewEvent&, uint64_t, std::stop_token)>;
  explicit RemoteViewInput(Send send) : send_(std::move(send)) {}
  ~RemoteViewInput() {
    for (auto& [id, route] : routes_) route.writer.request_stop();
  }

  auto push(const WorkerViewEvent& event) -> WorkerProc::ViewDelivery {
    std::lock_guard lock(mutex_);
    if (!listening_) return WorkerProc::ViewDelivery::closed;
    auto& route = routes_[event.route];
    if (route.closed) return WorkerProc::ViewDelivery::closed;
    const bool closing = event.kind == ViewEventKind::close;
    if (!closing && route.queued > kThreshold)
      return WorkerProc::ViewDelivery::full;
    if (!route.writer.joinable())
      route.writer = std::jthread(
          [this, &route](std::stop_token stop) { write(route, stop); });
    route.pending.push_back(event);
    if (closing)
      route.closed = true;
    else
      route.queued += workerd::message_cost(event.bytes.size());
    changed_.notify_all();
    return WorkerProc::ViewDelivery::accepted;
  }

  auto update(const RemoteJobState& status) -> void {
    std::lock_guard lock(mutex_);
    listening_ = status.state == "running" && status.view_input_ready;
    for (const auto& [id, released] : status.view_input_credits) {
      const auto route = routes_.find(id);
      if (route != routes_.end())
        route->second.released = std::max(route->second.released, released);
    }
    changed_.notify_all();
  }

 private:
  static constexpr size_t kThreshold =
      workerd::kViewBudget - TSUGUMI_MESSAGE_MAX;
  struct Route {
    std::deque<WorkerViewEvent> pending;
    bool closed = false;
    size_t queued = 0;
    uint64_t sent = 0;
    uint64_t released = 0;
    std::jthread writer;
  };

  auto write(Route& route, std::stop_token stop) -> void {
    std::unique_lock lock(mutex_);
    const auto ready = [&] {
      // Close can always enter the queue and can be sent at its head without
      // taking any byte credit. Earlier messages keep their FIFO order.
      return listening_ && !route.pending.empty() &&
             (route.pending.front().kind == ViewEventKind::close ||
              route.sent <= route.released + kThreshold);
    };
    while (changed_.wait(lock, stop, ready) && !stop.stop_requested()) {
      auto event = std::move(route.pending.front());
      route.pending.pop_front();
      const bool closing = event.kind == ViewEventKind::close;
      const auto charge =
          closing ? 0 : workerd::message_cost(event.bytes.size());
      const auto offset = route.sent;
      lock.unlock();
      bool accepted = false;
      bool retry = false;
      try {
        send_(event, offset, stop);
        accepted = true;
      } catch (const BrokerDisconnected&) {
        retry = true;  // The worker deduplicates by the byte offset or close.
      } catch (const std::exception&) {
      }
      lock.lock();
      if (retry) {
        route.pending.push_front(std::move(event));
        changed_.wait_for(lock, stop, std::chrono::milliseconds(50),
                          [] { return false; });
      } else {
        if (closing) return;
        route.queued -= charge;
        if (accepted) route.sent += charge;
      }
    }
  }

  Send send_;
  std::mutex mutex_;
  std::condition_variable_any changed_;
  bool listening_ = false;
  std::map<ViewUuid, Route> routes_;
};

auto poll_remote_job(const RemoteJobClient& client, RemoteJobRecord& job,
                     uint64_t log_offset, uint64_t display_after,
                     const std::map<ViewUuid, uint64_t>& credits,
                     const std::optional<WorkerViewReply>& view_reply,
                     std::stop_token stop) -> RemoteJobState {
  const auto& s = job.invocation;
  const auto fingerprint = crypto::sha256_hex(job.spec);
  RemoteJobState reply;
  if (job.cancel_requested) {
    reply = client.cancel(s.execution_service, job.instance, s.execution_job_id,
                          fingerprint, log_offset);
  } else {
    reply = client.status(s.execution_service, job.instance, s.execution_job_id,
                          log_offset, stop, display_after, credits, view_reply);
    if (reply.state == "missing" || reply.state == "uploading") {
      if (job.accepted)
        return {"interrupted",
                fingerprint,
                {},
                "the execution service lost a previously accepted job"};
      reply = client.submit(s.execution_service, job.instance,
                            s.execution_job_id, job.spec, stop);
    } else if (reply.state == "queued" && reply.cache_settings_wanted) {
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

auto external_cache_kind(Tsugumi::RemoteWorker::CacheTargetKind target)
    -> ExternalCacheKind {
  using Kind = Tsugumi::RemoteWorker::CacheTargetKind;
  switch (target) {
    case Kind::Attic:
      return ExternalCacheKind::attic;
    case Kind::Cachix:
      return ExternalCacheKind::cachix;
    case Kind::NixCopy:
      return ExternalCacheKind::nix_copy;
    case Kind::Own:
      break;
  }
  throw std::runtime_error("missing built-in binary cache connection");
}

// Publishes the part of `path`'s closure that no pull cache serves (and, for
// the built-in cache, that it lacks), references first.
auto publish_closure(const RemoteJobClient& client, const std::string& service,
                     const Tsugumi::RemoteWorker::CachePushSettingsT& push,
                     const std::string& store, const std::string& path,
                     std::stop_token stop,
                     const CacheTransferReport& report = {}) -> CachePushPlan {
  if (push.kind == Tsugumi::RemoteWorker::CacheTargetKind::Own)
    return publish_to_relay_cache(
        store, path,
        [&](auto method, const auto& name, auto size, auto upload,
            auto download) {
          return client.cache_file(service, method, name, size,
                                   std::move(upload), std::move(download),
                                   stop);
        },
        report);
  const ExternalCacheTarget target{external_cache_kind(push.kind),
                                   push.endpoint, push.cache_name, push.user,
                                   push.secret};
  // Nix copy takes the whole closure; it skips what the target holds.
  if (target.kind == ExternalCacheKind::nix_copy) {
    publish_to_external_cache(target, stdfs::temp_directory_path(),
                              {.store = store}, {path}, stop);
    return {};
  }
  auto plan = plan_store_push(store, path);
  if (!report) {
    publish_to_external_cache(target, stdfs::temp_directory_path(),
                              {.store = store}, plan.paths, stop);
    return plan;
  }
  // One upload per path, so the status can count.
  CacheTransferProgress progress{0, plan.paths.size(), 0, plan.bytes, {}};
  for (const auto& entry : plan.paths) {
    progress.path = entry;
    report(progress);
    publish_to_external_cache(target, stdfs::temp_directory_path(),
                              {.store = store}, {entry}, stop);
    ++progress.done;
  }
  progress.path.clear();
  progress.bytes_done = plan.bytes;
  report(progress);
  return plan;
}

auto publish_remote_derivation(const RemoteJobClient& client,
                               const std::string& service,
                               const std::string& store,
                               const std::string& path, std::stop_token stop)
    -> std::string {
  auto settings = client.cache_settings(service, stop);
  publish_closure(client, service, *settings.push, store, path, stop);
  return settings.revision;
}

// The scheduler selected this worker before evaluating its derivation.
class PreparedRemoteExecution final : public PreparedExecution {
 public:
  PreparedRemoteExecution(Service& service, const RemoteJobClient& client,
                          std::string target, std::string system,
                          std::string instance, std::stop_token stop,
                          DocumentPtr doc)
      : service_(service),
        client_(client),
        target_(std::move(target)),
        system_(std::move(system)),
        instance_(std::move(instance)),
        transport_([this, stop](const auto& store, const auto& path) {
          cache_revision_ =
              publish_remote_derivation(client_, target_, store, path, stop);
        }),
        doc_(std::move(doc)) {}
  PreparedRemoteExecution(const PreparedRemoteExecution&) = delete;
  auto operator=(const PreparedRemoteExecution&)
      -> PreparedRemoteExecution& = delete;

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
  std::string cache_revision_;
  BinaryCacheDerivationTransport transport_;
  DocumentPtr doc_;
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
  const bool probe = request.build.card_probe;
  s.execution_service = target_;
  s.execution_job_id =
      probe ? crypto::sha256_hex(crypto::uuid4())
            : crypto::sha256_hex(service_.project_id() + "\n" + s.run_id +
                                 "\n" + path_key(node.path) + "\n" +
                                 std::to_string(s.invocation));
  auto spec =
      stage_remote_invocation(client_, target_, instance_, s.execution_job_id,
                              cache_revision_, request);
  if (request.stop.stop_requested())
    throw Failed(FailureKind::cancelled,
                 "run cancelled before external submission");

  s.execution_system = system_;
  if (settings.sandbox.limits.limited())
    s.execution_resources = settings.sandbox.limits;
  s.execution_state = RemoteExecutionState::queued;
  s.worker_reused = false;
  RemoteJobRecord job{s, instance_, std::move(spec)};
  // Cancellation identity is durable before submission; results are never
  // recovered after restart.
  if (!probe) {
    request.start([&](sql::Db& db) {
      job.invocation = s;
      job.cancel_requested = request.cancel_requested();
      History::put_remote_job(db, job);
    });
  }
  struct ReleaseProbe {
    const RemoteJobClient& client;
    const RemoteJobRecord& job;
    bool enabled;
    bool terminal = false;
    ~ReleaseProbe() {
      if (!enabled) return;
      const auto& invocation = job.invocation;
      const auto fingerprint = crypto::sha256_hex(job.spec);
      try {
        if (!terminal)
          client.cancel(invocation.execution_service, job.instance,
                        invocation.execution_job_id, fingerprint);
        client.release(invocation.execution_service, job.instance,
                       invocation.execution_job_id, fingerprint);
      } catch (const std::exception& error) {
        log::warn("cannot release remote Card probe: " +
                  std::string(error.what()));
      }
    }
  } release_probe{client_, job, probe};

  RemoteViewInput input(
      [&client = client_, target = target_, instance = instance_,
       id = s.execution_job_id](const auto& message, uint64_t offset,
                                std::stop_token stop) {
        client.send_view(target, instance, id, message, offset, stop);
      });
  auto receiver = std::make_shared<WorkerProc::ViewReceiver>();
  receiver->remote = [&input](const WorkerViewEvent& event,
                              WorkerProc::DisplayCredit) {
    return input.push(event);
  };
  struct CloseReceiver {
    WorkerProc::ViewReceiver& receiver;
    ~CloseReceiver() { receiver.close(); }
  } close_receiver{*receiver};
  struct DisplayProgress {
    std::mutex mutex;
    std::map<ViewUuid, uint64_t> released;
  };
  auto display = std::make_shared<DisplayProgress>();
  uint64_t display_after = 0;
  std::optional<WorkerViewReply> last_view_reply;
  bool view_reply_pending = false;
  std::optional<std::vector<schema::CardView>> views;
  auto& history = service_.history();
  ExecutionResult output;
  LiveRemoteLog live_log(
      probe ? kProbeTransportBytes : kRemoteLogTransportBytes, request.log);
  bool secrets_sent = false;
  std::mutex wait_mutex;
  std::condition_variable_any next_status;
  while (!request.detach.stop_requested()) {
    const auto before = s.execution_state;
    const auto activity_before = s.execution_activity;
    const auto detail_before = s.execution_activity_detail;
    const auto cpu_before = s.cpu_ms;
    const auto building_before = s.building.size();
    job.cancel_requested |= request.cancel_requested();
    const auto stop = job.cancel_requested ? request.detach : request.stop;
    std::optional<RemoteJobState> reply;
    bool more_log = false;
    try {
      std::map<ViewUuid, uint64_t> credits;
      {
        std::lock_guard lock(display->mutex);
        credits = display->released;
      }
      reply = poll_remote_job(
          client_, job, live_log.cursor(), display_after, credits,
          view_reply_pending ? last_view_reply : std::nullopt, stop);
      if (stop.stop_requested()) continue;
      more_log = live_log.accept(*reply);
      input.update(*reply);
      view_reply_pending = false;
      if (const auto& control = reply->view_request) {
        if (!last_view_reply ||
            control->request_id > last_view_reply->request_id) {
          if (control->kind == ViewControlKind::open && !views) {
            // View blobs are served from this Core's native store. Evaluate
            // only the declarations, from the submitted run's snapshot.
            try {
              BuildRequest resolve;
              resolve.node = &node;
              resolve.doc = doc_;
              resolve.timeout_seconds = node.settings.build_timeout_seconds;
              resolve.cancel = stop;
              views = service_.nix().declared_views(resolve);
            } catch (const Failed&) {
              if (stop.stop_requested()) continue;
              views.emplace();
            }
          }
          static const std::vector<schema::CardView> empty;
          last_view_reply =
              service_.view_control(Service::ViewOrigin(s),
                                    views ? *views : empty, receiver, *control);
        }
        view_reply_pending = last_view_reply->request_id == control->request_id;
      }
      if (reply->display_sequence > display_after) {
        display_after = reply->display_sequence;
        for (auto& message : reply->displays) {
          message.source = receiver;
          const auto release =
              [display, session = message.session,
               cost = workerd::message_cost(message.bytes.size())] {
                std::lock_guard lock(display->mutex);
                display->released[session] += cost;
              };
          if (request.display)
            request.display(std::move(message), release);
          else
            release();
        }
      }
      if (!reply->artifact_wanted.empty() && !job.cancel_requested) {
        const auto id = parse_artifact_id(reply->artifact_wanted);
        if (!id) throw BrokerRequestRejected("invalid remote artifact UUID");
        deliver_remote_artifact(client_, target_, instance_, s.execution_job_id,
                                reply->fingerprint, reply->artifact_wanted,
                                service_.resolve_artifact(*id), stop);
      }
      s.execution_state = remote_execution_state(reply->state);
      s.execution_activity = execution_activity(*reply);
      s.execution_activity_detail =
          s.execution_activity == ExecutionActivity::unspecified
              ? std::string{}
              : reply->activity_detail.substr(0, 256);
      if (reply->state == "running") {
        s.cpu_ms = reply->cpu_ms;
        // Keep when each derivation started, as the core saw it start.
        std::vector<BuildActivityData> building;
        for (const auto& name : reply->building) {
          if (building.size() == 64) break;
          const auto found =
              std::ranges::find(s.building, name, &BuildActivityData::derivation);
          building.push_back(found != s.building.end()
                                 ? *found
                                 : BuildActivityData{name.substr(0, 256), now_ms()});
        }
        s.building = std::move(building);
      } else {
        s.building.clear();
      }
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
    if (s.execution_state != before ||
        s.execution_activity != activity_before ||
        s.execution_activity_detail != detail_before || s.cpu_ms != cpu_before ||
        s.building.size() != building_before)
      request.publish();
    if (reply && reply->state == "succeeded") {
      // A stopped job keeps its workspace until release: collect the rest
      // of its log first. Each poll is a bounded exchange that advances the
      // cursor and stays interruptible as before.
      release_probe.terminal = true;
      if (probe && reply->log_truncated) {
        if (more_log) continue;
        output.failure = FailureData::make(FailureKind::worker_failed,
                                           "Probe output exceeded the limit");
        return output;
      }
      if (reply->display_pending) {
        if (reply->displays.empty()) {
          std::unique_lock lock(wait_mutex);
          next_status.wait_for(lock, stop, std::chrono::milliseconds(50),
                               [] { return false; });
        }
        continue;
      }
      if (job.cancel_requested) {
        if (more_log) continue;
        output.failure =
            FailureData::make(FailureKind::cancelled, "Run cancelled");
        return output;
      }
      try {
        auto result = import_remote_result(
            client_, target_, instance_, s.execution_job_id,
            reply->result_sha256, job.spec,
            probe ? request.directory
                  : service_.store().state_dir() / "artifact-invocations",
            probe ? stdfs::path{} : service_.artifact_destination(s),
            node.outputs, live_log.recorded(), request.log, request.stop,
            [&](std::string_view name, uint64_t done, uint64_t total) {
              if (request.transfer)
                request.transfer("download", name, done, total);
            },
            request.on_remote_validation);
        if (!probe && !result.environment.paths.empty()) {
          s.build_cached = !result.built;
          s.build_substituter = std::move(result.substituter);
          s.build_pushed = result.pushed;
          s.build_push = std::move(result.push);
          history.write_sync([&](sql::Db& db) {
            s.environment_id =
                History::put_environment(db, s.run_id, result.environment);
          });
        }
        if (!probe) service_.record_artifacts(s, result.created);
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
      release_probe.terminal = true;
      output.failure = remote_failure(*reply);
      return output;
    }
    // A log behind the job's own continues without waiting.
    if (more_log || (reply && (!reply->displays.empty() || view_reply_pending)))
      continue;
    // The worker protocol reports progress by polling. Stop interrupts the
    // interval and the exchange so Cancel can use a new connection immediately.
    std::unique_lock lock(wait_mutex);
    const auto interval = reply && reply->display_pending ? 50 : 750;
    next_status.wait_for(lock, stop, std::chrono::milliseconds(interval),
                         [] { return false; });
  }
  throw ExecutionDetached{};
}

}  // namespace

RemoteExecutionService::RemoteExecutionService(Service& service,
                                               RemoteJobClient& client)
    : service_(service), client_(client) {}

auto RemoteExecutionService::publish_local_build(
    const WorkerDerivation& derivation, bool push_outputs,
    InvocationSummaryData& s, const std::function<void()>& publish,
    std::stop_token stop) -> void {
  if (!client_.enabled()) return;
  s.build_pushed = false;
  const auto report = [&](const CacheTransferProgress& progress) {
    s.execution_activity = ExecutionActivity::pushing;
    s.execution_activity_detail = format_progress(progress);
    // /nix/store/<hash>-<name> -> "<name> (progress)"
    if (const auto dash = progress.path.find('-', progress.path.rfind('/') + 1);
        !progress.path.empty() && dash != std::string::npos)
      s.execution_activity_detail = progress.path.substr(dash + 1) + " (" +
                                    s.execution_activity_detail + ")";
    if (publish) publish();
  };
  Tsugumi::Data::BuildPushT outcome;
  try {
    const auto settings = client_.cache_settings(kLocalExecutionService, stop);
    try {
      publish_closure(client_, kLocalExecutionService, *settings.push, {},
                      derivation.path, stop);
    } catch (const std::exception&) {
      log::warn(
          "Could not publish the local Nix derivation to the binary cache");
    }
    if (push_outputs) {
      const auto plan =
          publish_closure(client_, kLocalExecutionService, *settings.push, {},
                          derivation.output, stop, report);
      outcome.paths = plan.paths.size();
      outcome.bytes = plan.bytes;
      outcome.skipped = plan.served + plan.present;
      s.build_pushed = true;
    }
  } catch (const std::exception& error) {
    outcome.error = std::string(error.what()).substr(0, 512);
  }
  if (push_outputs) s.build_push = std::move(outcome);
  s.execution_activity = ExecutionActivity::unspecified;
  s.execution_activity_detail.clear();
}
auto RemoteExecutionService::prepare(const RemoteTarget& target,
                                     const Node& node,
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
  if (build.cancel.stop_requested())
    throw Failed(FailureKind::cancelled, "worker selection cancelled");
  auto probe = client_.probe(target.id, target.server_id, build.cancel);
  const auto system = build.probe_only
                          ? probe.native_system
                          : run_system(settings, probe.native_system);
  if (settings.sandbox.limits.limited() && !probe.resource_limits)
    throw Failed(FailureKind::unsupported,
                 "the remote execution daemon cannot enforce resource limits");
  if (std::ranges::find(probe.systems, system) == probe.systems.end())
    throw Failed(
        FailureKind::unsupported,
        "external execution service no longer supports system " + system);
  stdfs::create_directories(directory);
  auto prepared = std::make_unique<PreparedRemoteExecution>(
      service_, client_, target.id, system, std::move(probe.instance),
      build.cancel, build.doc);
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
