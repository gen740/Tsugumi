// Realises and runs one accepted invocation inside its supervised namespace.
#include "tsugumi/workerd/invocation.hh"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

#include "display_channel.hh"
#include "nix_store.hh"
#include "tsugumi/protocol/worker_generated.h"
#include "tsugumi/rpc/frame.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/system.hh"
#include "tsugumi/workerd/artifact_id.hh"
#include "tsugumi/workerd/card_plan.hh"
#include "tsugumi/workerd/frame.hh"
#include "tsugumi/workerd/log.hh"
#include "tsugumi/workerd/log_limits.hh"
#include "tsugumi/workerd/protocol.hh"
#include "tsugumi/workerd/staging.hh"
#include "tsugumi/workerd/store_result.hh"
#include "tsugumi/workerd/value_wire.hh"
#include "tsugumi/workerd/worker_execution.hh"

namespace tsugumi {
namespace {

namespace W = Tsugumi::Worker;
namespace stdfs = std::filesystem;

constexpr uint64_t kWorkerLimit = workerd::kMaxWorkerFrameBytes;
// How long a built invocation waits for its Card secrets from the core.
constexpr auto kSecretWait = std::chrono::minutes(5);

using Secrets = std::shared_ptr<WorkerSecrets>;

// Reads the job's Card secrets from the listener's pipe once the environment
// is built and nothing but the Card's process remains to start. The values
// stay in memory; the marker file only tells the core that the job waits.
auto receive_secrets(const stdfs::path& directory,
                     const workerd::R::JobSpecT& job) -> Secrets {
  const auto marker = directory / workerd::kSecretsWanted;
  fs::write_atomic(marker, "", 0600);
  std::string frame;
  frame.reserve(workerd::kMaxFrame + 4);
  bool closed = false;
  const auto deadline = std::chrono::steady_clock::now() + kSecretWait;
  while (!closed && frame.size() <= workerd::kMaxFrame + 4) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) break;
    pollfd ready{workerd::kSecretChannelFd, POLLIN, 0};
    const int polled = ::poll(&ready, 1, static_cast<int>(left.count()));
    if (polled < 0 && errno == EINTR) continue;
    if (polled <= 0) break;
    char buffer[4096];
    const auto n = ::read(workerd::kSecretChannelFd, buffer, sizeof buffer);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0)
      closed = true;
    else {
      frame.append(buffer, static_cast<size_t>(n));
      workerd::wipe_secret({buffer, static_cast<size_t>(n)});
    }
  }
  ::close(workerd::kSecretChannelFd);
  std::error_code ec;
  stdfs::remove(marker, ec);
  if (frame.empty()) {
    if (closed)
      throw Failed(FailureKind::worker_failed,
                   "the remote worker restarted before the Card started; "
                   "Card secrets are kept only in memory, so they were lost. "
                   "Run the Card again");
    throw Failed(FailureKind::worker_failed,
                 "the Card's secrets did not arrive within 5 minutes. Run the "
                 "Card again");
  }
  auto secrets = std::make_shared<WorkerSecrets>();
  try {
    if (!closed || frame.size() < 4 ||
        workerd::frame_length(std::string_view(frame).substr(0, 4)) !=
            frame.size() - 4)
      throw std::runtime_error("truncated");
    auto message = workerd::decode(std::string_view(frame).substr(4));
    auto* reply = message->payload.AsReply();
    auto* body = reply ? reply->body.AsCardSecrets() : nullptr;
    if (!body) throw std::runtime_error("unexpected message");
    secrets->values.reserve(body->secrets.size());
    for (auto& secret : body->secrets) {
      secrets->values.emplace_back(std::move(secret->name), secret->value);
      workerd::clear_secret(secret->value);
    }
  } catch (const std::exception&) {
    workerd::clear_secret(frame);
    throw Failed(FailureKind::worker_failed,
                 "the Card's secrets arrived incomplete. Run the Card again");
  }
  workerd::clear_secret(frame);
  std::vector<std::string> wanted = job.secrets, given;
  for (const auto& secret : secrets->values) given.push_back(secret.first);
  std::ranges::sort(wanted);
  std::ranges::sort(given);
  if (wanted != given) {
    throw Failed(FailureKind::worker_failed,
                 "the Card's secrets do not match the names it declares");
  }
  return secrets;
}

auto read(const stdfs::path& path, uint64_t limit) -> std::string {
  if (stdfs::file_size(path) > limit)
    throw std::runtime_error("remote invocation member is too large");
  auto data = fs::read_file(path);
  if (!data) throw std::runtime_error("remote invocation member is missing");
  return std::move(*data);
}

auto bytes(const flatbuffers::Vector<uint8_t>* value) -> std::string {
  return {reinterpret_cast<const char*>(value->data()), value->size()};
}

auto make_result(const workerd::InvocationResult& outcome) -> std::string {
  flatbuffers::FlatBufferBuilder b;
  W::WorkerPayload kind;
  flatbuffers::Offset<void> body;
  if (outcome.failure) {
    kind = W::WorkerPayload::WorkerFailure;
    body =
        W::CreateWorkerFailure(b, wire::failure(b, *outcome.failure),
                               wire::artifact_ids(b, outcome.created)).Union();
  } else {
    std::vector<flatbuffers::Offset<W::WorkerOutput>> outputs;
    for (const auto& value : outcome.outputs)
      outputs.push_back(W::CreateWorkerOutput(
          b, b.CreateString(value.name),
          b.CreateVector(reinterpret_cast<const uint8_t*>(value.bytes.data()),
                         value.bytes.size()),
          wire::artifact_ids(b, value.artifacts)));
    kind = W::WorkerPayload::WorkerResult;
    body = W::CreateWorkerResult(b, b.CreateVector(outputs),
                                 wire::artifact_ids(b, outcome.created)).Union();
  }
  W::FinishWorkerMessageBuffer(b, W::CreateWorkerMessage(b, 1, kind, body));
  if (b.GetSize() > kWorkerLimit)
    throw std::runtime_error("remote worker result exceeds 16 MiB");
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}

// The native worker's credit is held until the remote listener forwards the
// message. Its socket reader never waits for a slow view or a network poll.
class InvocationDisplay {
 public:
  explicit InvocationDisplay(std::weak_ptr<WorkerProc::ViewReceiver> target)
      : target_(std::move(target)), reader_([this] { read(); }) {
    try {
      writer_ = std::thread([this] { write(); });
    } catch (...) {
      ::shutdown(workerd::kDisplayChannelFd, SHUT_RDWR);
      reader_.join();
      throw;
    }
  }
  ~InvocationDisplay() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_all();
    writer_.join();
    ::shutdown(workerd::kDisplayChannelFd, SHUT_RDWR);
    reader_.join();
  }

  auto push(WorkerDisplayData display, WorkerProc::DisplayCredit complete)
      -> void {
    std::unique_lock lock(mutex_);
    if (closed_) {
      lock.unlock();
      complete();
      return;
    }
    pending_.emplace_back(std::move(display), std::move(complete));
    ready_.notify_one();
  }

  auto request(const WorkerViewRequest& request)
      -> std::optional<WorkerViewReply> {
    std::lock_guard lock(mutex_);
    if (closed_ || stopping_) return WorkerViewReply{request.request_id, false};
    request_ = request;
    outstanding_request_ = request.request_id;
    ready_.notify_one();
    return std::nullopt;
  }

 private:
  auto write() -> void {
    try {
      const char ready = 1;
      if (::write(workerd::kDisplayChannelFd, &ready, 1) != 1)
        throw std::runtime_error("cannot signal remote display readiness");
      for (;;) {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] {
          return stopping_ || closed_ || request_ || !pending_.empty() ||
                 !credits_.empty();
        });
        if (closed_) return;
        if (request_) {
          auto request = std::exchange(request_, std::nullopt);
          lock.unlock();
          flatbuffers::FlatBufferBuilder b;
          workerd::build_view_request(b, *request);
          workerd::write_display_frame(workerd::kDisplayChannelFd, b);
          continue;
        }
        if (!credits_.empty()) {
          auto credit = credits_.extract(credits_.begin());
          lock.unlock();
          workerd::write_display_credit(workerd::kDisplayChannelFd,
                                        credit.key(), credit.mapped());
          continue;
        }
        if (stopping_ && pending_.empty()) return;
        auto [display, complete] = std::move(pending_.front());
        pending_.pop_front();
        in_flight_[display.session].emplace_back(
            workerd::message_cost(display.bytes.size()), std::move(complete));
        lock.unlock();
        flatbuffers::FlatBufferBuilder b;
        workerd::build_display(b, display.session, display.bytes);
        workerd::write_display_frame(workerd::kDisplayChannelFd, b);
      }
    } catch (const std::exception&) {
      ::shutdown(workerd::kDisplayChannelFd, SHUT_RDWR);
    }
  }

  auto read() -> void {
    try {
      while (auto frame = workerd::read_display(workerd::kDisplayChannelFd)) {
        const auto* envelope = W::GetWorkerMessage(frame->data());
        if (const auto* response = envelope->value_as_WorkerViewReply()) {
          {
            std::lock_guard lock(mutex_);
            if (response->request_id() == 0 ||
                response->request_id() != outstanding_request_)
              break;
            outstanding_request_ = 0;
          }
          if (auto worker = target_.lock())
            worker->reply(workerd::view_reply(*response));
          continue;
        }
        if (const auto* message = envelope->value_as_WorkerViewEvent()) {
          auto event = workerd::view_event(*message);
          if (!workerd::valid_view_event(event)) break;
          WorkerProc::DisplayCredit complete;
          if (event.kind == ViewEventKind::message) {
            complete = [this, route = event.route,
                        cost = workerd::message_cost(event.bytes.size())] {
              std::lock_guard lock(mutex_);
              credits_[route] += cost;
              ready_.notify_one();
            };
          }
          // Continue to return credits while the worker retires.
          const auto worker = target_.lock();
          const auto delivery = worker ? worker->send(event, complete)
                                       : WorkerProc::ViewDelivery::closed;
          if (delivery == WorkerProc::ViewDelivery::full)
            throw std::runtime_error(
                "remote event exceeded the worker relay credit");
          if (delivery == WorkerProc::ViewDelivery::closed && complete)
            complete();
          continue;
        }
        const auto* credit = envelope->value_as_WorkerDisplayCredit();
        if (!credit) break;
        size_t bytes = credit->bytes();
        std::unique_lock lock(mutex_);
        auto& view = in_flight_[workerd::view_uuid(credit->address())];
        while (bytes) {
          if (view.empty() || bytes < view.front().first)
            throw std::runtime_error("invalid remote display credit");
          auto [cost, complete] = std::move(view.front());
          view.pop_front();
          bytes -= cost;
          lock.unlock();
          complete();
          lock.lock();
        }
      }
    } catch (const std::exception&) {
    }
    ::shutdown(workerd::kDisplayChannelFd, SHUT_RDWR);
    std::unique_lock lock(mutex_);
    closed_ = true;
    const auto request_id = std::exchange(outstanding_request_, 0);
    auto in_flight = std::move(in_flight_);
    auto pending = std::move(pending_);
    ready_.notify_all();
    lock.unlock();
    if (request_id)
      if (auto worker = target_.lock()) worker->reply({request_id, false});
    for (auto& [name, view] : in_flight)
      for (auto& [cost, complete] : view) complete();
    for (auto& [display, complete] : pending) complete();
  }

  std::weak_ptr<WorkerProc::ViewReceiver> target_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::pair<WorkerDisplayData, WorkerProc::DisplayCredit>> pending_;
  std::optional<WorkerViewRequest> request_;
  uint64_t outstanding_request_ = 0;
  std::map<ViewUuid, std::deque<std::pair<size_t, WorkerProc::DisplayCredit>>>
      in_flight_;
  bool stopping_ = false;
  bool closed_ = false;
  std::map<ViewUuid, size_t> credits_;
  std::thread reader_;
  std::thread writer_;
};

auto run_worker(const W::WorkerCall& call, const workerd::R::JobSpecT& job,
                const remote::RW::InvocationT& invocation,
                const stdfs::path& input, const stdfs::path& directory,
                const stdfs::path& store, const stdfs::path& group,
                const WorkerStoreResult& env, const WorkerProc::LogSink& log,
                Secrets secrets) -> workerd::InvocationResult {
  schema::CardPlan plan;
  if (schema::decode_card_plan(env.plan, plan))
    throw Failed(FailureKind::nix_failed, "invalid remote Card build plan");
  const auto index = invocation.implementation_index;
  const auto& runs = *schema::get(plan.runs);
  const auto* worker_binary = schema::get(plan.worker);
  if (index == 0 || index > runs.size() || !worker_binary ||
      !worker_binary->starts_with("/nix/store/") ||
      !runs.at(index - 1).starts_with("/nix/store/"))
    throw Failed(FailureKind::nix_failed,
                 "remote Card build plan lacks a package or worker");
  WorkerSpawn spawn;
  spawn.worker_binary = *worker_binary;
  spawn.package = runs.at(index - 1);
  spawn.secrets = std::move(secrets);
  spawn.closure = env.closure;
  spawn.cgroup = group;
  spawn.store_path = (store / "nix/store").string();
  spawn.scratch = (directory / "scratch").string();
  spawn.sandbox = {job.filesystem,
                   job.pid,
                   job.ipc,
                   job.identity,
                   {static_cast<uint32_t>(job.cpu_millis), job.memory_bytes}};
  spawn.network = job.network;
  if (spawn.sandbox.identity) spawn.uid = spawn.gid = 65534;

  std::vector<Value> values;
  for (const auto* port : *call.inputs())
    values.push_back({port->name()->str(), nullptr,
                      port->bytes() ? bytes(port->bytes()) : "",
                      port->schema() ? port->schema()->str() : "",
                      wire::artifact_ids(port->artifacts())});
  std::vector<types::PortType> outputs;
  for (const auto* port : *call.outputs())
    outputs.push_back({port->name()->str(), port->required(), nullptr});
  const auto artifacts_root = input / "artifacts";
  const auto outputs_root = directory / "result/files/artifacts";
  spawn.artifact_store = outputs_root.string();
  spawn.artifact_staging = directory / "artifact-invocations";
  std::vector<ArtifactGrant> grants;
  if (const auto* entries = call.grants()) {
    for (const auto* entry : *entries) {
      auto id = wire::artifact_id(*entry->id());
      const bool store_origin = entry->store_path() != nullptr;
      const auto leaf =
          store_origin
              ? crypto::hex(id.data(), id.size())
              : std::to_string(artifact_counter(artifact_id_bytes(id)));
      const auto relative = remote::relative_path(entry->path()->string_view());
      const auto holders = relative.parent_path();
      const auto invocation_name = holders.filename().string();
      const auto source = artifacts_root / relative;
      if (relative.filename() != leaf ||
          !crypto::is_run_id(relative.begin()->string()) ||
          !invocation_name.starts_with('@') || invocation_name.size() == 1 ||
          invocation_name.find_first_not_of("0123456789", 1) !=
              std::string::npos ||
          !stdfs::is_directory(stdfs::symlink_status(source)))
        throw Failed(FailureKind::input_missing, "invalid remote artifact grant");
      std::string store_target;
      if (const auto* store_path = entry->store_path()) {
        const stdfs::path path(store_path->str());
        const stdfs::path store_root = "/nix/store";
        const auto relative_store_path = path.lexically_relative(store_root);
        if (relative_store_path.empty() ||
            !fs::valid_relative_path(relative_store_path.begin()->string()) ||
            path != store_root / *relative_store_path.begin() /
                        "share/tsugumi/input/artifacts" / leaf)
          throw Failed(FailureKind::input_missing,
                       "invalid store artifact path");
        if (spawn.sandbox.filesystem) {
          spawn.binds.push_back({source.string(), path.string(), false, ""});
          store_target = path.string();
        } else {
          store_target = source.string();
        }
      }
      grants.push_back(
          {std::move(id), source.string(), {}, std::move(store_target)});
    }
  }
  stdfs::create_directories(outputs_root);
  auto worker = std::make_unique<WorkerProc>(spawn);
  InvocationDisplay display(worker->view_receiver());
  // Retire the worker and its pending display callbacks before the relay.
  WorkerPool::Lease lease;
  lease.worker = std::move(worker);
  spawn.secrets.reset();
  workerd::Invocation request;
  request.inputs = values;
  request.outputs = std::move(outputs);
  request.artifact_directory = outputs_root;
  request.artifact_sequence = call.invocation_sequence();
  request.grants = std::move(grants);
  request.output_budget = {job.output_limit_bytes, job.output_limit_entries};
  if (call.deadline_ms()) request.timeout_ms = *call.deadline_ms();
  // Allow the worker to report its own budget failure before escalation.
  request.deadline_grace = std::chrono::milliseconds(500);
  request.log = log;
  request.display = [&display](auto message, auto complete) {
    display.push(std::move(message), std::move(complete));
  };
  request.view_control = [&display](const auto& control) {
    return display.request(control);
  };
  return workerd::run_invocation(
      [](std::unique_ptr<WorkerProc>, bool, const std::string&) {}, lease,
      false, request);
}

}  // namespace

namespace workerd {
auto invoke(const stdfs::path& directory, const stdfs::path& blobs,
            const stdfs::path& store, const stdfs::path& group) -> void {
  const auto job = parse_spec(read(directory / "spec.fb", kMaxSpec));
  if (::fcntl(kDisplayChannelFd, F_SETFD, FD_CLOEXEC) != 0)
    throw std::runtime_error("the job's live display channel is missing");
  // Nothing this process starts may inherit the Card secret pipe.
  if (!job->secrets.empty() &&
      ::fcntl(kSecretChannelFd, F_SETFD, FD_CLOEXEC) != 0)
    throw std::runtime_error("the job's Card secret channel is missing");
  const auto& derivation = job->derivation;
  const auto& output = job->output;
  const std::set<std::string> accepted(job->input_blobs.begin(),
                                       job->input_blobs.end());
  const auto blob = [&](const std::string& digest) -> stdfs::path {
    if (!accepted.contains(digest))
      throw std::runtime_error("remote input blob was not submitted");
    return blobs / digest;
  };
  const auto decoded = remote::decode<remote::RW::Invocation>(
      read(blob(job->invocation_sha256), kWorkerLimit));
  if (!decoded) throw std::runtime_error("invalid remote invocation manifest");
  const auto& invocation = *decoded;
  int64_t last_progress = 0;
  const auto progress = [&](const stdfs::path& name, uint64_t done,
                            uint64_t total) {
    const auto now = now_ms();
    if (done && done != total && now - last_progress < 100) return;
    last_progress = now;
    remote::RW::TransferProgressT value;
    value.name = name.generic_string();
    value.done = done;
    value.total = total;
    fs::write_atomic(directory / "progress",
                     remote::encode<remote::RW::TransferProgress>(value), 0600);
  };
  const auto input = directory / "staged";
  stdfs::create_directories(input);
  for (const auto& dir : invocation.directories)
    stdfs::create_directories(input / remote::relative_path(dir));
  for (const auto& entry : invocation.files) {
    const auto& file = *entry;
    const auto path = input / remote::relative_path(file.path);
    const auto source = blob(file.sha256);
    stdfs::create_directories(path.parent_path());
    uint64_t done = 0;
    if (const auto size = fs::link_beneath(blobs, source.filename(), path)) {
      done = *size;
      progress(file.path, done, file.size);
    } else {
      progress(file.path, 0, file.size);
      fs::copy_beneath(blobs, source.filename(), path,
                       [&](std::string_view chunk) {
                         done += chunk.size();
                         progress(file.path, done, file.size);
                       });
    }
    if (done != file.size)
      throw std::runtime_error("remote invocation input size does not match");
  }
  const auto frame = read(blob(invocation.call_sha256), kWorkerLimit);
  flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(frame.data()),
                                 frame.size());
  if (!W::VerifyWorkerMessageBuffer(verifier))
    throw std::runtime_error("invalid remote invocation call");
  const auto* call = W::GetWorkerMessage(frame.data())->value_as_WorkerCall();
  if (!call)
    throw std::runtime_error("remote invocation does not contain a Card call");

  const auto result_dir = directory / "result";
  stdfs::create_directories(result_dir);
  std::ofstream logs(result_dir / "logs", std::ios::binary);
  InvocationLog invocation_log(kRemoteLogTransportBytes);
  const auto truncation_marker = directory / "log-truncated";
  auto log = [&](LogStream stream, const std::string& text) {
    if (invocation_log.truncated()) return;
    if (auto entry = invocation_log.append(stream, text)) {
      // Flush each entry so status requests can collect it while it runs.
      logs << *entry << std::flush;
    } else {
      const int fd = ::open(truncation_marker.c_str(),
                            O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (fd >= 0) ::close(fd);
    }
  };
  WorkerStoreResult environment;
  InvocationResult outcome;
  // Only definition data enter this persistent private Nix store. Invocation
  // inputs and results remain in job scratch, outside the store.
  try {
    environment = RemoteWorkerStore::realise(
        ArchiveDerivationTransport(blob(invocation.archive_sha256)), store,
        WorkerDerivation{derivation, output, job->system},
        [&](const std::string& line) { log(LogStream::build, line); },
        job->build_timeout_ms);
    auto secrets =
        job->secrets.empty() ? Secrets{} : receive_secrets(directory, *job);
    outcome = run_worker(*call, *job, invocation, input, directory, store,
                         group, environment, log, std::move(secrets));
  } catch (const Failed& error) {
    outcome.failure = error.failure;
  } catch (const std::exception& error) {
    outcome.failure = FailureData::make(environment.derivation.output.empty()
                                            ? FailureKind::nix_failed
                                            : FailureKind::worker_failed,
                                        error.what());
  }
  remote::RW::ResultEnvironmentT info;
  info.paths = std::move(environment.closure);
  info.built = environment.built;
  info.log_truncated = invocation_log.truncated();
  const auto files = result_dir / "files";
  if (stdfs::is_directory(files)) {
    for (const auto& entry : stdfs::recursive_directory_iterator(files))
      if (stdfs::is_directory(entry.symlink_status()))
        info.directories.push_back(
            entry.path().lexically_relative(files).generic_string());
  }
  fs::write_atomic(result_dir / "environment",
                   remote::encode<remote::RW::ResultEnvironment>(info), 0600);
  fs::write_atomic(result_dir / "worker.fb", make_result(outcome), 0600);
}

}  // namespace workerd
}  // namespace tsugumi
