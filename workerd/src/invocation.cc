#include "tsugumi/workerd/manifest.hh"
// Realises and runs one accepted invocation inside its supervised namespace.
#include <fcntl.h>
#include <flatbuffers/reflection.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>

#include "nix_store.hh"
#include "tsugumi/protocol/worker_generated.h"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/system.hh"
#include "tsugumi/workerd/frame.hh"
#include "tsugumi/workerd/invocation.hh"
#include "tsugumi/workerd/log.hh"
#include "tsugumi/workerd/module_source.hh"
#include "tsugumi/workerd/protocol.hh"
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

auto type(const flatbuffers::Vector<uint8_t>* schema,
          const flatbuffers::String* root) -> types::TypeRef {
  if (!schema || !root) throw std::runtime_error("missing remote port schema");
  flatbuffers::Verifier verifier(schema->data(), schema->size());
  if (!reflection::VerifySchemaBuffer(verifier))
    throw std::runtime_error("invalid remote port schema");
  auto result = std::make_shared<types::Type>();
  result->bfbs = bytes(schema);
  result->root_type = root->str();
  if (!result->root()) throw std::runtime_error("unknown remote port type");
  return result;
}

auto make_result(const workerd::InvocationResult& outcome) -> std::string {
  flatbuffers::FlatBufferBuilder b;
  W::WorkerPayload kind;
  flatbuffers::Offset<void> body;
  if (outcome.failure) {
    kind = W::WorkerPayload::WorkerFailure;
    body =
        W::CreateWorkerFailure(b, wire::failure(b, *outcome.failure)).Union();
  } else {
    std::vector<flatbuffers::Offset<W::WorkerOutput>> outputs;
    for (const auto& value : outcome.outputs) {
      auto data =
          b.CreateVector(reinterpret_cast<const uint8_t*>(value.bytes.data()),
                         value.bytes.size());
      outputs.push_back(
          W::CreateWorkerOutput(b, b.CreateString(value.name), data));
    }
    kind = W::WorkerPayload::WorkerResult;
    body = W::CreateWorkerResult(b, b.CreateVector(outputs)).Union();
  }
  W::FinishWorkerMessageBuffer(b, W::CreateWorkerMessage(b, 1, kind, body));
  if (b.GetSize() > kWorkerLimit)
    throw std::runtime_error("remote worker result exceeds 16 MiB");
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}

auto run_worker(const W::WorkerCall& call, const workerd::R::JobSpecT& job,
                const remote::InvocationJson& invocation,
                const stdfs::path& input, const stdfs::path& directory,
                const stdfs::path& store, const stdfs::path& group,
                const WorkerStoreResult& env, const WorkerProc::LogSink& log,
                const WorkerProc::DisplaySink& display,
                const fs::TransferProgress& progress, Secrets secrets)
    -> workerd::InvocationResult {
  schema::CardManifest manifest;
  if (schema::decode(env.manifest, manifest))
    throw Failed(FailureKind::nix_failed, "invalid remote Card manifest");
  const auto* single = schema::get(manifest.run);
  const auto* runs = schema::get(manifest.runs);
  const auto index = *invocation.implementation_index;
  const schema::ManifestRun* run = nullptr;
  if (single && !runs && index == 1) run = single;
  if (!single && runs && index > 0 && index <= runs->size())
    run = &runs->at(index - 1);
  const auto* shell = schema::get(manifest.shell);
  const auto* path = schema::get(manifest.path);
  const auto* worker_binary = run ? schema::get(run->worker) : nullptr;
  if (!run || !shell || !shell->starts_with("/nix/store/") || !path ||
      !worker_binary || !worker_binary->starts_with("/nix/store/"))
    throw Failed(FailureKind::nix_failed,
                 "remote Card manifest lacks a pinned shell or worker");
  WorkerSpawn spawn;
  spawn.worker_binary = *worker_binary;
  spawn.bash = *shell;
  spawn.path = *path;
  spawn.secrets = std::move(secrets);
  spawn.closure = env.closure;
  spawn.cgroup = group;
  spawn.store_path = (store / "nix/store").string();
  spawn.scratch = (directory / "scratch").string();
  spawn.program.package = *run->package;
  spawn.program.bash = *run->bash;
  if (spawn.program.package.has_value() == spawn.program.bash.has_value())
    throw Failed(FailureKind::nix_failed,
                 "remote Card manifest must contain one package or Bash body");
  spawn.program.pre_run = manifest.pre_run->value_or("");
  spawn.program.post_run = manifest.post_run->value_or("");
  spawn.program.views = manifest.views->value_or("");
  spawn.sandbox = {job.filesystem,
                   job.pid,
                   job.ipc,
                   job.identity,
                   {static_cast<uint32_t>(job.cpu_millis), job.memory_bytes}};
  spawn.network = job.network;
  if (spawn.sandbox.identity) spawn.uid = spawn.gid = 65534;
  spawn.ro_paths = {input.string()};
  CardPath card;
  for (const auto* name : *call.card()->segments()) card.push_back(name->str());
  spawn.card = path_key(card);

  std::vector<Value> values;
  for (const auto* port : *call.inputs()) {
    const auto* data = port->value();
    Value value{port->name()->str(),
                port->kind(),
                type(data->schema_bfbs(), data->root_type()),
                bytes(data->bytes()),
                {}};
    if (value.kind != PortKind::FlatBuffer) {
      const auto logical = path_value(value);
      if (!logical) throw std::runtime_error("remote input value lacks a path");
      const auto rel = remote::relative_path(*logical);
      if (*rel.begin() != "inputs")
        throw std::runtime_error("remote input path is outside staging");
      value = with_path(value, (input / rel).string());
    }
    values.push_back(std::move(value));
  }
  std::vector<types::PortType> outputs;
  for (const auto* port : *call.outputs())
    outputs.push_back({port->name()->str(), port->kind(),
                       port->optional().value_or(false),
                       type(port->schema_bfbs(), port->root_type())});
  WorkerPool pool(directory / "scratch", {}, WorkerPoolMode::one_shot);
  auto lease = pool.acquire(spawn.card, "", spawn, false);
  spawn.secrets.reset();
  workerd::Invocation request;
  request.run_id = call.run_id()->str();
  request.card = card;
  request.number = call.invocation().value_or(0);
  request.inputs = values;
  request.outputs = std::move(outputs);
  request.params.bfbs = bytes(call.params()->schema_bfbs());
  request.params.root = call.params()->root_type()->str();
  request.params.bytes = bytes(call.params()->bytes());
  if (spawn.program.bash)
    for (const auto* v : *call.environment())
      if (v->name()->string_view() == "TSUGUMI_PARAMS_JSON")
        request.params.json = v->value()->str();
  request.output_directory = directory / "result/files/out";
  request.input_directory = input;
  request.delivery = workerd::OutputDelivery::transferred_files;
  if (call.deadline_ms()) request.timeout_ms = *call.deadline_ms();
  // Allow the worker to report its own budget failure before escalation.
  request.deadline_grace = std::chrono::milliseconds(500);
  request.log = log;
  request.transfer = progress;
  request.display = display;
  return workerd::run_invocation(
      [&](auto worker, bool keep, const auto& reason) {
        pool.release(spawn.card, lease.generation, std::move(worker), keep,
                     reason);
      },
      lease, false, request);
}

}  // namespace

namespace workerd {
auto invoke(const stdfs::path& directory, const stdfs::path& blobs,
            const stdfs::path& store, const stdfs::path& group) -> void {
  const auto job = parse_spec(read(directory / "spec.fb", kMaxSpec));
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
  remote::InvocationJson invocation;
  if (schema::decode(read(blob(job->invocation_sha256), kWorkerLimit),
                     invocation))
    throw std::runtime_error("invalid remote invocation manifest");
  int64_t last_progress = 0;
  const auto progress = [&](const stdfs::path& name, uint64_t done,
                            uint64_t total) {
    const auto now = now_ms();
    if (done && done != total && now - last_progress < 100) return;
    last_progress = now;
    remote::TransferJson value;
    *value.name = name.generic_string();
    *value.done = done;
    *value.total = total;
    fs::write_atomic(directory / "progress.json", schema::encode(value), 0600);
  };
  const auto input = directory / "staged";
  stdfs::create_directories(input);
  for (const auto& dir : *invocation.directories)
    stdfs::create_directories(input / remote::relative_path(dir));
  for (const auto& file : *invocation.files) {
    const auto path = input / remote::relative_path(*file.path);
    const auto source = blob(*file.sha256);
    stdfs::create_directories(path.parent_path());
    uint64_t done = 0;
    if (const auto size = fs::link_beneath(blobs, source.filename(), path)) {
      done = *size;
      progress(*file.path, done, *file.size);
    } else {
      progress(*file.path, 0, *file.size);
      fs::copy_beneath(blobs, source.filename(), path,
                       [&](std::string_view chunk) {
                         done += chunk.size();
                         progress(*file.path, done, *file.size);
                       });
    }
    if (done != *file.size)
      throw std::runtime_error("remote invocation input size does not match");
  }
  const auto frame = read(blob(*invocation.call_sha256), kWorkerLimit);
  flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(frame.data()),
                                 frame.size());
  if (!W::VerifyWorkerMessageBuffer(verifier))
    throw std::runtime_error("invalid remote invocation call");
  const auto* call = W::GetWorkerMessage(frame.data())->value_as_WorkerCall();
  if (!call)
    throw std::runtime_error("remote invocation does not contain a Card call");

  const auto result_dir = directory / "result";
  stdfs::create_directories(result_dir);
  std::ofstream logs(result_dir / "logs.jsonl", std::ios::binary);
  InvocationLog invocation_log(job->log_limit_bytes);
  const auto truncation_marker = directory / "log-truncated";
  auto log = [&](LogStream stream, const std::string& text) {
    if (invocation_log.truncated()) return;
    if (auto entry = invocation_log.append(stream, text)) {
      // Flush each entry so status requests can collect it while it runs.
      logs << *entry << '\n' << std::flush;
    } else {
      const int fd = ::open(truncation_marker.c_str(),
                            O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (fd >= 0) ::close(fd);
    }
  };
  std::string display;
  WorkerStoreResult environment;
  InvocationResult outcome;
  // Only definition data enter this persistent private Nix store. Invocation
  // inputs and results remain in job scratch, outside the store.
  try {
    environment = RemoteWorkerStore::realise(
        ArchiveDerivationTransport(blob(*invocation.archive_sha256)), store,
        WorkerDerivation{derivation, output, job->system},
        [&](const std::string& line) { log(LogStream::build, line); },
        job->build_timeout_ms);
    auto secrets =
        job->secrets.empty() ? Secrets{} : receive_secrets(directory, *job);
    outcome = run_worker(
        *call, *job, invocation, input, directory, store, group, environment,
        log,
        [&](const DisplayData& data) {
          display = wire::standalone_display(data);
        },
        progress, std::move(secrets));
    if (!display.empty()) {
      const auto* module =
          flatbuffers::GetRoot<Tsugumi::Data::Display>(display.data())
              ->value_as_DisplayModule();
      if (module && module->module_()) {
        try {
          const auto identity = module->module_()->str();
          if (!valid_module_identity(identity) ||
              std::ranges::find(environment.closure, identity) ==
                  environment.closure.end())
            throw std::runtime_error(
                "display module is outside the built closure");
          // Transport a built definition resource under its immutable identity.
          // The core imports it without interpreting the display's run data.
          const auto store_directory = store / "nix/store";
          const auto basename = stdfs::path(identity).filename();
          const auto root = store_directory / basename;
          if (stdfs::is_symlink(root) || !stdfs::is_directory(root))
            throw std::runtime_error("invalid display module output");
          fs::write_atomic(result_dir / "modules" / basename / "index.js",
                           module_source(root, store_directory), 0600);
        } catch (const std::exception&) {
          // A view failure must not replace the Card result.
        }
      }
    }
  } catch (const Failed& error) {
    outcome.failure = error.failure;
  } catch (const std::exception& error) {
    outcome.failure = FailureData::make(environment.derivation.output.empty()
                                            ? FailureKind::nix_failed
                                            : FailureKind::worker_failed,
                                        error.what());
  }
  remote::EnvironmentJson info;
  *info.paths = std::move(environment.closure);
  *info.built = environment.built;
  *info.log_truncated = invocation_log.truncated();
  *info.directories = std::vector<std::string>{};
  const auto files = result_dir / "files";
  if (outcome.failure) stdfs::remove_all(files);
  if (stdfs::is_directory(files)) {
    for (const auto& entry : stdfs::recursive_directory_iterator(files))
      if (stdfs::is_directory(entry.symlink_status()))
        info.directories->push_back(
            entry.path().lexically_relative(files).generic_string());
  }
  if (!display.empty())
    fs::write_atomic(result_dir / "display.fb", display, 0600);
  fs::write_atomic(result_dir / "environment.json", schema::encode(info), 0600);
  fs::write_atomic(result_dir / "worker.fb", make_result(outcome), 0600);
}

}  // namespace workerd
}  // namespace tsugumi
