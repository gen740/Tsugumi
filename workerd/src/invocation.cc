#include "tsugumi/workerd/manifest.hh"
// Realises and runs one accepted invocation inside its supervised namespace.
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>

#include <flatbuffers/reflection.h>

#include "tsugumi/workerd/worker_execution.hh"
#include "tsugumi/workerd/module_source.hh"
#include "nix_store.hh"
#include "tsugumi/workerd/store_result.hh"
#include "tsugumi/workerd/invocation.hh"
#include "tsugumi/workerd/log.hh"
#include "tsugumi/workerd/protocol.hh"
#include "tsugumi/workerd/frame.hh"
#include "tsugumi/protocol/worker_generated.h"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/system.hh"
#include "tsugumi/workerd/value_wire.hh"

namespace tsugumi {
namespace {

namespace W = Tsugumi::Worker;
namespace stdfs = std::filesystem;

constexpr uint64_t kWorkerLimit = workerd::kMaxWorkerFrameBytes;

auto read(const stdfs::path& path, uint64_t limit) -> std::string {
  if (stdfs::file_size(path) > limit) throw std::runtime_error("remote invocation member is too large");
  auto data = fs::read_file(path);
  if (!data) throw std::runtime_error("remote invocation member is missing");
  return std::move(*data);
}

auto bytes(const flatbuffers::Vector<uint8_t>* value) -> std::string {
  return {reinterpret_cast<const char*>(value->data()), value->size()};
}

auto type(const flatbuffers::Vector<uint8_t>* schema, const flatbuffers::String* root) -> types::TypeRef {
  if (!schema || !root) throw std::runtime_error("missing remote port schema");
  flatbuffers::Verifier verifier(schema->data(), schema->size());
  if (!reflection::VerifySchemaBuffer(verifier)) throw std::runtime_error("invalid remote port schema");
  auto result = std::make_shared<types::Type>();
  result->bfbs = bytes(schema);
  result->root_type = root->str();
  if (!result->root()) throw std::runtime_error("unknown remote port type");
  return result;
}

auto make_result(const WorkerOutcome& outcome) -> std::string {
  flatbuffers::FlatBufferBuilder b;
  W::WorkerPayload kind;
  flatbuffers::Offset<void> body;
  if (outcome.failure) {
    kind = W::WorkerPayload::WorkerFailure;
    body = W::CreateWorkerFailure(b, wire::failure(b, *outcome.failure)).Union();
  } else {
    std::vector<flatbuffers::Offset<W::WorkerOutput>> outputs;
    for (const auto& [name, value] : outcome.outputs) {
      auto data = b.CreateVector(reinterpret_cast<const uint8_t*>(value.data()), value.size());
      outputs.push_back(W::CreateWorkerOutput(b, b.CreateString(name), data));
    }
    kind = W::WorkerPayload::WorkerResult;
    body = W::CreateWorkerResult(b, b.CreateVector(outputs)).Union();
  }
  W::FinishWorkerMessageBuffer(b, W::CreateWorkerMessage(b, 1, kind, body));
  if (b.GetSize() > kWorkerLimit) throw std::runtime_error("remote worker result exceeds 16 MiB");
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}

auto run_worker(const W::WorkerCall& call, const remote::InvocationJson& invocation, const stdfs::path& input,
                const stdfs::path& directory, const stdfs::path& store, const WorkerStoreResult& env,
                const WorkerProc::LogSink& log, const WorkerProc::DisplaySink& display,
                const fs::TransferProgress& progress) -> WorkerOutcome {
  schema::CardManifest manifest;
  if (schema::decode(env.manifest, manifest)) throw Failed(FailureKind::nix_failed, "invalid remote Card manifest");
  const auto* single = schema::get(manifest.run);
  const auto* runs = schema::get(manifest.runs);
  const auto index = *invocation.implementation_index;
  const schema::ManifestRun* run = nullptr;
  if (single && !runs && index == 1) run = single;
  if (!single && runs && index > 0 && index <= runs->size()) run = &runs->at(index - 1);
  const auto* shell = schema::get(manifest.shell);
  const auto* path = schema::get(manifest.path);
  const auto* worker_binary = run ? schema::get(run->worker) : nullptr;
  if (!run || !shell || !shell->starts_with("/nix/store/") || !path || !worker_binary ||
      !worker_binary->starts_with("/nix/store/"))
    throw Failed(FailureKind::nix_failed, "remote Card manifest lacks a pinned shell or worker");
  WorkerSpawn spawn;
  spawn.worker_binary = *worker_binary;
  spawn.bash = *shell;
  spawn.store_path = (store / "nix/store").string();
  spawn.scratch = (directory / "scratch").string();
  spawn.program.package = *run->package;
  spawn.program.bash = *run->bash;
  if (spawn.program.package.has_value() == spawn.program.bash.has_value())
    throw Failed(FailureKind::nix_failed, "remote Card manifest must contain one package or Bash body");
  spawn.program.pre_run = manifest.pre_run->value_or("");
  spawn.program.post_run = manifest.post_run->value_or("");
  spawn.program.views = manifest.views->value_or("");
  spawn.network = *invocation.network;
  spawn.uid = 65534;
  spawn.gid = 65534;
  spawn.ro_paths = {input.string()};
  CardPath card;
  for (const auto* name : *call.card()->segments()) card.push_back(name->str());
  spawn.card = path_key(card);

  std::vector<Value> values;
  for (const auto* port : *call.inputs()) {
    const auto* data = port->value();
    Value value{port->name()->str(), port->kind(), type(data->schema_bfbs(), data->root_type()), bytes(data->bytes()), {}};
    if (!types::verify(*value.type, value.bytes)) throw std::runtime_error("invalid remote input value");
    if (value.kind != PortKind::FlatBuffer) {
      const auto logical = path_value(value);
      if (!logical) throw std::runtime_error("remote input value lacks a path");
      const auto rel = remote::relative_path(*logical);
      if (*rel.begin() != "inputs") throw std::runtime_error("remote input path is outside staging");
      value = with_path(value, (input / rel).string());
    }
    values.push_back(std::move(value));
  }
  std::vector<types::PortType> outputs;
  for (const auto* port : *call.outputs())
    outputs.push_back({port->name()->str(), port->kind(), port->optional().value_or(false),
        type(port->schema_bfbs(), port->root_type())});
  WorkerPool pool(directory / "scratch", {}, WorkerPoolMode::one_shot);
  auto lease = pool.acquire(spawn.card, "", spawn, false);
  workerd::Invocation request;
  request.run_id = call.run_id()->str();
  request.card = card;
  request.number = call.invocation().value_or(0);
  request.inputs = values;
  request.outputs = std::move(outputs);
  request.params.bfbs = bytes(call.params()->schema_bfbs());
  request.params.root = call.params()->root_type()->str();
  request.params.bytes = bytes(call.params()->bytes());
  request.path = *path;
  request.worker_binary = spawn.worker_binary;
  for (const auto* v : *call.environment())
    if (v->name()->string_view() == "TSUGUMI_PARAMS_JSON") request.environment.emplace_back(v->name()->str(), v->value()->str());
  request.output_directory = directory / "result/files/out";
  request.input_directory = input;
  request.delivery = workerd::OutputDelivery::transferred_files;
  request.stop_before_outputs = true;
  if (call.deadline_ms()) request.timeout_ms = *call.deadline_ms();
  request.deadline_grace = std::chrono::milliseconds(500);
  request.log = log;
  request.transfer = progress;
  request.display = display;
  return workerd::run_invocation([&](auto worker, bool keep, const auto& reason) {
    pool.release(spawn.card, "", std::move(worker), keep, reason);
  }, lease, false, request).outcome;
}

}  // namespace

namespace workerd {
auto invoke(const stdfs::path& directory, const stdfs::path& blobs, const stdfs::path& store) -> void {
  const auto job = parse_spec(read(directory / "spec.fb", kMaxSpec));
  const auto& derivation = job->derivation;
  const auto& output = job->output;
  if (job->system != native_system()) throw std::runtime_error("remote job system mismatch");
  const std::set<std::string> accepted(job->input_blobs.begin(), job->input_blobs.end());
  const auto blob = [&](const std::string& digest) -> stdfs::path {
    if (!crypto::is_sha256_hex(digest) || !accepted.contains(digest)) throw std::runtime_error("remote input blob was not submitted");
    return blobs / digest;
  };
  remote::InvocationJson invocation;
  if (schema::decode(read(blob(job->invocation_sha256), kWorkerLimit), invocation))
    throw std::runtime_error("invalid remote invocation manifest");
  int64_t last_progress = 0;
  const auto progress = [&](const stdfs::path& name, uint64_t done, uint64_t total) {
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
    if (stdfs::file_size(source) != *file.size) throw std::runtime_error("remote input size mismatch");
    stdfs::create_directories(path.parent_path());
    uint64_t done = 0;
    progress(*file.path, 0, *file.size);
    fs::copy_beneath(blobs, *file.sha256, path, [&](std::string_view chunk) {
      done += chunk.size(); progress(*file.path, done, *file.size);
    });
  }
  const auto frame = read(blob(*invocation.call_sha256), kWorkerLimit);
  flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
  if (!W::VerifyWorkerMessageBuffer(verifier)) throw std::runtime_error("invalid remote worker invocation");
  const auto* call = W::GetWorkerMessage(frame.data())->value_as_WorkerCall();
  if (!call) throw std::runtime_error("remote invocation is not a worker call");

  const auto result_dir = directory / "result";
  stdfs::create_directories(result_dir);
  std::ofstream logs(result_dir / "logs.jsonl", std::ios::binary);
  InvocationLog invocation_log(logs, job->log_limit_bytes);
  auto log = [&](LogStream stream, const std::string& text) {
    invocation_log.append(static_cast<uint64_t>(stream), text);
  };
  std::string display;
  WorkerStoreResult environment;
  WorkerOutcome outcome;
  // Only definition data enter this persistent private Nix store. Invocation
  // inputs and results remain in job scratch, outside the store.
  try {
    environment = RemoteWorkerStore::realise(ArchiveDerivationTransport(blob(*invocation.archive_sha256)), store,
        WorkerDerivation{derivation, output, job->system},
        [&](const std::string& line) { log(LogStream::build, line + "\n"); }, job->build_timeout_ms);
    outcome = run_worker(*call, invocation, input, directory, store, environment, log,
        [&](const DisplayData& data) { display = wire::standalone_display(data); }, progress);
    if (!display.empty()) {
      const auto* module = flatbuffers::GetRoot<Tsugumi::Data::Display>(display.data())->value_as_DisplayModule();
      if (module && module->module_()) {
        try {
          const auto identity = module->module_()->str();
          if (!valid_module_identity(identity) ||
              std::ranges::find(environment.closure, identity) == environment.closure.end())
            throw std::runtime_error("display module is outside the built closure");
          // Transport a built definition resource under its immutable identity.
          // The core imports it without interpreting the display's run data.
          const auto store_directory = store / "nix/store";
          const auto basename = stdfs::path(identity).filename();
          const auto root = store_directory / basename;
          if (stdfs::is_symlink(root) || !stdfs::is_directory(root))
            throw std::runtime_error("invalid display module output");
          fs::write_atomic(result_dir / "modules" / basename / "index.js", module_source(root, store_directory), 0600);
        } catch (const std::exception&) {
          // A view failure must not replace the Card result.
        }
      }
    }
  } catch (const Failed& error) {
    outcome.failure = error.failure;
  } catch (const std::exception& error) {
    outcome.failure = FailureData::make(environment.derivation.output.empty() ? FailureKind::nix_failed : FailureKind::worker_failed, error.what());
  }
  invocation_log.finish();
  remote::EnvironmentJson info;
  *info.paths = std::move(environment.closure);
  *info.built = environment.built;
  *info.directories = std::vector<std::string>{};
  const auto files = result_dir / "files";
  if (outcome.failure) stdfs::remove_all(files);
  if (stdfs::is_directory(files)) {
    for (const auto& entry : stdfs::recursive_directory_iterator(files))
      if (stdfs::is_directory(entry.symlink_status())) info.directories->push_back(entry.path().lexically_relative(files).generic_string());
  }
  if (!display.empty()) fs::write_atomic(result_dir / "display.fb", display, 0600);
  fs::write_atomic(result_dir / "environment.json", schema::encode(info), 0600);
  fs::write_atomic(result_dir / "worker.fb", make_result(outcome), 0600);
}

}  // namespace workerd
}  // namespace tsugumi
