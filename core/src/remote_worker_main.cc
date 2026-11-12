// Executes one authenticated, staged remote invocation. The launcher owns
// the journal and cgroup; Card code only gets the usual worker sandbox.
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>

#include <flatbuffers/reflection.h>

#include "remote_invocation.hh"
#include "tsugumi/protocol/worker_generated.h"
#include "util/crypto.hh"
#include "util/fs.hh"
#include "wire.hh"

namespace tsugumi {
namespace {

namespace W = Tsugumi::Worker;
namespace stdfs = std::filesystem;

constexpr uint64_t kWorkerLimit = uint64_t{16} << 20;
constexpr size_t kLogLimit = size_t{8} << 20;
constexpr int32_t kOutSlot = 900;

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

auto copy_tree(const stdfs::path& source, const stdfs::path& destination) -> void {
  if (stdfs::is_regular_file(stdfs::symlink_status(source))) {
    stdfs::create_directories(destination.parent_path());
    stdfs::copy_file(source, destination);
    return;
  }
  if (!stdfs::is_directory(stdfs::symlink_status(source)))
    throw Failed(FailureKind::output_invalid, "remote output must be a regular file or directory");
  stdfs::create_directories(destination);
  for (const auto& entry : stdfs::recursive_directory_iterator(source)) {
    const auto path = destination / entry.path().lexically_relative(source);
    const auto status = entry.symlink_status();
    if (stdfs::is_directory(status)) stdfs::create_directory(path);
    else if (stdfs::is_regular_file(status)) stdfs::copy_file(entry.path(), path);
    else throw Failed(FailureKind::output_invalid, "remote output directory contains a symlink or special file");
  }
}

auto run_worker(const W::WorkerCall& call, const remote::InvocationJson& invocation, const stdfs::path& input,
                const stdfs::path& directory, const stdfs::path& store, const NixResult& env,
                const WorkerProc::LogSink& log, const WorkerProc::DisplaySink& display) -> WorkerOutcome {
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
  spawn.build.package = *run->package;
  spawn.build.bash = *run->bash;
  if (spawn.build.package.has_value() == spawn.build.bash.has_value())
    throw Failed(FailureKind::nix_failed, "remote Card manifest must contain one package or Bash body");
  spawn.build.pre_run = manifest.pre_run->value_or("");
  spawn.build.post_run = manifest.post_run->value_or("");
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
  WorkerProc worker(spawn);
  if (!worker.passes_directories()) throw Failed(FailureKind::permission_denied, "remote worker requires bubblewrap");
  const auto out = directory / "result/files/out";
  const bool needs_out = std::ranges::any_of(outputs, [](const auto& port) { return port.kind != PortKind::FlatBuffer; });
  int out_fd = -1;
  const std::string out_alias = "/proc/self/fd/" + std::to_string(kOutSlot);
  if (needs_out) {
    stdfs::create_directories(out);
    out_fd = ::open(out.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (out_fd < 0) throw std::runtime_error("cannot open remote output directory");
  }
  struct Close {
    int fd;
    ~Close() { if (fd >= 0) ::close(fd); }
  } close{out_fd};
  std::vector<std::pair<std::string, std::string>> environment{
      {"PATH", *path}, {"HOME", worker.work_dir()}, {"LANG", "C.UTF-8"},
      {"TSUGUMI_RUN_ID", call.run_id()->str()}, {"TSUGUMI_CARD", spawn.card},
      {"TSUGUMI_INVOCATION", std::to_string(call.invocation().value_or(0))},
      {"TSUGUMI_WORKER", spawn.worker_binary},
  };
  for (const auto* v : *call.environment())
    if (v->name()->string_view() == "TSUGUMI_PARAMS_JSON") environment.emplace_back(v->name()->str(), v->value()->str());
  const auto id = worker.next_call_id();
  auto frame = worker_call_frame(id, call.run_id()->str(), card, call.invocation().value_or(0), values,
      bytes(call.params()->schema_bfbs()), call.params()->root_type()->str(), bytes(call.params()->bytes()), outputs,
      worker.work_dir(), worker.tmp_dir(), needs_out ? out_alias : "", environment, {},
      needs_out ? std::vector<int32_t>{kOutSlot} : std::vector<int32_t>{});
  worker.set_logs(log);
  auto outcome = worker.call(frame, id, needs_out ? std::vector<int>{out_fd} : std::vector<int>{}, nullptr, {}, display);
  // Nothing written by the Card can change once we inspect its output tree.
  worker.terminate();
  if (outcome.failure) return outcome;
  stdfs::path forwarded;
  for (auto& [name, data] : outcome.outputs) {
    const auto port = std::ranges::find(outputs, name, &types::PortType::name);
    if (port == outputs.end() || !types::verify(*port->type, data))
      throw Failed(FailureKind::output_invalid, "invalid remote worker output " + name);
    if (port->kind == PortKind::FlatBuffer) continue;
    Value value{name, port->kind, port->type, data, {}};
    const auto actual = path_value(value);
    if (!actual) throw Failed(FailureKind::output_invalid, "remote output has no path");
    stdfs::path rel;
    if (*actual == out_alias) {
      rel = "out";
    } else if (actual->starts_with(out_alias + "/")) {
      rel = "out" / remote::relative_path(std::string_view(*actual).substr(out_alias.size() + 1));
    } else if (std::ranges::any_of(values, [&](const Value& in) { return in.kind == port->kind && path_value(in) == actual; })) {
      if (forwarded.empty()) {
        forwarded = stdfs::path("out") / (".forwarded-" + crypto::uuid4());
        if (!stdfs::create_directory(directory / "result/files" / forwarded))
          throw Failed(FailureKind::output_invalid, "cannot create a private forwarded output directory");
      }
      rel = forwarded / std::to_string(port - outputs.begin()) / stdfs::path(*actual).filename();
      copy_tree(*actual, directory / "result/files" / rel);
    } else {
      throw Failed(FailureKind::output_invalid, "remote output points outside its output directory");
    }
    const auto actual_path = directory / "result/files" / rel;
    if (!fs::resolves_inside(out, actual_path) && actual_path != out)
      throw Failed(FailureKind::output_invalid, "remote output escapes its directory");
    data = with_path(value, rel.generic_string()).bytes;
  }
  if (*invocation.filesystem) {
    copy_tree(worker.work_dir(), directory / "result/files/work");
    copy_tree(worker.tmp_dir(), directory / "result/files/tmp");
  }
  return outcome;
}

auto invoke(const stdfs::path& directory, const stdfs::path& blobs) -> void {
  remote::JobJson job;
  if (schema::decode(read(directory / "spec.json", 256 * 1024), job) || *job.system != native_system() ||
      !remote::store_path(*job.derivation) || !job.derivation->ends_with(".drv") || !remote::store_path(*job.output))
    throw std::runtime_error("invalid remote Nix job for this system");
  const std::set<std::string> accepted(job.input_blobs->begin(), job.input_blobs->end());
  const auto blob = [&](const std::string& digest) -> stdfs::path {
    if (!crypto::is_sha256_hex(digest) || !accepted.contains(digest)) throw std::runtime_error("remote input blob was not submitted");
    return blobs / digest;
  };
  remote::InvocationJson invocation;
  if (schema::decode(read(blob(*job.invocation_sha256), kWorkerLimit), invocation) || !crypto::is_uuid(*invocation.project_id))
    throw std::runtime_error("invalid remote invocation manifest");
  const auto input = directory / "staged";
  stdfs::create_directories(input);
  for (const auto& dir : *invocation.directories)
    stdfs::create_directories(input / remote::relative_path(dir));
  for (const auto& file : *invocation.files) {
    const auto path = input / remote::relative_path(*file.path);
    const auto source = blob(*file.sha256);
    if (stdfs::file_size(source) != *file.size) throw std::runtime_error("remote input size mismatch");
    stdfs::create_directories(path.parent_path());
    stdfs::copy_file(source, path);
  }
  const auto frame = read(blob(*invocation.call_sha256), kWorkerLimit);
  flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
  if (!W::VerifyWorkerMessageBuffer(verifier)) throw std::runtime_error("invalid remote worker invocation");
  const auto* call = W::GetWorkerMessage(frame.data())->value_as_WorkerCall();
  if (!call) throw std::runtime_error("remote invocation is not a worker call");

  const auto result_dir = directory / "result";
  stdfs::create_directories(result_dir);
  std::ofstream logs(result_dir / "logs.jsonl", std::ios::binary);
  size_t logged = 0;
  auto log = [&](LogStream stream, const std::string& text) {
    if (text.size() > kLogLimit - logged) return;
    remote::LogJson entry;
    *entry.stream = static_cast<uint64_t>(stream);
    *entry.text = text;
    const auto encoded = schema::encode(entry);
    if (encoded.size() + 1 > kLogLimit - logged) return;
    logs << encoded << '\n';
    logged += encoded.size() + 1;
  };
  std::string display;
  NixResult environment;
  WorkerOutcome outcome;
  // Only this invocation's store is mounted into the worker. It cannot read
  // source paths or outputs realised for another invocation or project.
  const auto store = directory / "nix-root";
  try {
    environment = NixEvaluator::realise_remote(blob(*invocation.archive_sha256), store, *job.derivation, *job.output,
        [&](const std::string& line) { log(LogStream::build, line + "\n"); });
    outcome = run_worker(*call, invocation, input, directory, store, environment, log,
        [&](const DisplayData& data) { display = wire::standalone_display(data); });
  } catch (const Failed& error) {
    outcome.failure = error.failure;
  } catch (const std::exception& error) {
    outcome.failure = FailureData::make(environment.out.empty() ? FailureKind::nix_failed : FailureKind::worker_failed, error.what());
  }
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

}  // namespace
}  // namespace tsugumi

auto main(int argc, char** argv) -> int {
  if (argc != 3) {
    std::cerr << "usage: tsugumi-remote-worker JOB_DIRECTORY BLOB_DIRECTORY\n";
    return 2;
  }
  try {
    tsugumi::NixEvaluator::init_process();
    tsugumi::invoke(argv[1], argv[2]);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
