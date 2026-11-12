#include "remote_invocation.hh"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <flatbuffers/reflection.h>

#include "tsugumi/protocol/worker_generated.h"
#include "util/crypto.hh"
#include "util/fs.hh"
#include "wire.hh"

namespace tsugumi {
namespace stdfs = std::filesystem;
namespace W = Tsugumi::Worker;

auto remote::relative_path(std::string_view path) -> stdfs::path {
  if (path.empty() || path.size() > 4096 || path.front() == '/' || path.back() == '/' ||
      path.find("//") != std::string_view::npos || path.find('\\') != std::string_view::npos ||
      std::ranges::any_of(path, [](unsigned char c) { return c < 32 || c == 127; }))
    throw std::runtime_error("invalid remote file path");
  stdfs::path result(path);
  for (const auto& part : result)
    if (part == "." || part == "..") throw std::runtime_error("invalid remote file path");
  return result;
}

auto remote::store_path(std::string_view path) -> bool {
  constexpr std::string_view prefix = "/nix/store/";
  if (!path.starts_with(prefix)) return false;
  path.remove_prefix(prefix.size());
  return path.size() > 33 && path.size() < 256 && path[32] == '-' &&
         std::ranges::all_of(path.substr(0, 32), [](char c) {
           return std::string_view("0123456789abcdfghijklmnpqrsvwxyz").find(c) != std::string_view::npos;
         }) && std::ranges::all_of(path.substr(33), [](unsigned char c) {
           return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '+' || c == '.' || c == '=' || c == '?';
         });
}

namespace {

constexpr uint64_t kMaxBlobBytes = uint64_t{64} << 30;
constexpr uint64_t kMaxManifestBytes = uint64_t{8} << 20;
constexpr uint64_t kMaxWorkerBytes = uint64_t{16} << 20;
constexpr size_t kMaxInputBlobs = 1024;

struct PortJson {
  schema::t::String<"name"> name;
  schema::t::Uint64<"kind"> kind;
  schema::t::Bool<"optional"> optional;
  schema::t::String<"bfbs"> bfbs;
  schema::t::String<"root_type"> root_type;
};

struct RecordedInputJson {
  schema::t::Field<"port", schema::Obj<PortJson>> port;
  schema::t::String<"bytes"> bytes;
  schema::t::String<"artifact_id"> artifact_id;
};

struct RecoveryJson {
  schema::t::Array<"outputs", schema::Obj<PortJson>> outputs;
  schema::t::Array<"inputs", schema::Obj<RecordedInputJson>> inputs;
  schema::t::Array<"sources", schema::Obj<schema::InputSourceJson>> sources;
  schema::t::Bool<"record_io"> record_io;
  schema::t::String<"artifact_output"> artifact_output;
};

auto upload(const RemoteJobClient& client, const std::string& service, const std::string& server_id,
            const std::string& instance, const std::string& bytes, const std::atomic<bool>& cancel) -> std::string {
  const std::string digest = crypto::sha256_hex(bytes);
  std::istringstream stream(bytes);
  client.put_blob(service, server_id, instance, digest, bytes.size(), stream, &cancel);
  return digest;
}

auto download(const RemoteJobClient& client, const std::string& service, const std::string& server_id,
              const std::string& instance, const std::string& digest, uint64_t limit, const std::atomic<bool>& stop) -> std::string {
  if (!crypto::is_sha256_hex(digest)) throw std::runtime_error("invalid remote blob digest");
  std::ostringstream bytes;
  client.get_blob(service, server_id, instance, digest, bytes, limit, &stop);
  return bytes.str();
}

auto input_location(const stdfs::path& state, const std::string& path) -> std::pair<stdfs::path, stdfs::path> {
  for (const auto& root : {state / "runs", state / "inputs"}) {
    auto rel = stdfs::path(path).lexically_normal().lexically_relative(root);
    if (!rel.empty() && rel != "." && *rel.begin() != "..") return {root, rel};
  }
  throw std::runtime_error("remote file input is outside the run's folders");
}

auto log_result(const stdfs::path& path, const WorkerProc::LogSink& log) -> void {
  if (!log) return;
  std::ifstream stream(path, std::ios::binary);
  std::string line;
  while (std::getline(stream, line)) {
    remote::LogJson entry;
    if (schema::decode(line, entry) || *entry.stream < 1 || *entry.stream > 4)
      throw std::runtime_error("invalid remote log entry");
    log(static_cast<LogStream>(*entry.stream), *entry.text);
  }
}

}  // namespace

auto remote_recovery_contract(const Node& node, const std::vector<Value>& inputs,
                              const std::vector<InputSourceData>& sources) -> std::string {
  RecoveryJson contract;
  *contract.record_io = node.settings.record_io;
  *contract.artifact_output = node.settings.retain_artifact ? node.settings.artifact_output.value_or("") : "";
  *contract.outputs = std::vector<PortJson>{};
  *contract.inputs = std::vector<RecordedInputJson>{};
  *contract.sources = std::vector<schema::InputSourceJson>{};
  const auto encode = [](const types::PortType& value) {
    if (!value.type) throw std::runtime_error("unresolved external port type");
    PortJson port;
    *port.name = value.name;
    *port.kind = static_cast<uint64_t>(value.kind);
    *port.optional = value.optional;
    *port.bfbs = crypto::base64(value.type->bfbs);
    *port.root_type = value.type->root_type;
    return port;
  };
  for (const auto& output : node.outputs) contract.outputs->push_back(encode(output));
  if (node.settings.record_io) for (const auto& input : inputs) {
    RecordedInputJson value;
    *value.port = encode({input.name, input.kind, false, input.type});
    *value.bytes = crypto::base64(input.bytes);
    *value.artifact_id = input.artifact_id.value_or("");
    contract.inputs->push_back(std::move(value));
  }
  for (const auto& input : sources) {
    schema::InputSourceJson source;
    *source.port = input.port;
    *source.card = path_key(input.card);
    *source.invocation = input.invocation;
    contract.sources->push_back(std::move(source));
  }
  return schema::encode(contract);
}

auto remote_recovery_contract(const std::string& encoded) -> RemoteRecoveryContract {
  RecoveryJson contract;
  if (schema::decode(encoded, contract)) throw std::runtime_error("invalid saved remote output contract");
  RemoteRecoveryContract result{{}, {}, {}, *contract.record_io, *contract.artifact_output};
  const auto decode = [](const PortJson& port) -> types::PortType {
    auto type = std::make_shared<types::Type>();
    if (!crypto::base64_decode(*port.bfbs, type->bfbs)) throw std::runtime_error("invalid saved remote schema");
    flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(type->bfbs.data()), type->bfbs.size());
    if (!reflection::VerifySchemaBuffer(verifier)) throw std::runtime_error("invalid saved remote schema");
    type->root_type = *port.root_type;
    type->hash = crypto::sha256_hex(type->bfbs);
    if (!type->root() || *port.kind < 1 || *port.kind > 3) throw std::runtime_error("invalid saved remote port");
    return {*port.name, static_cast<PortKind>(*port.kind), *port.optional, std::move(type)};
  };
  for (const auto& port : *contract.outputs) result.outputs.push_back(decode(port));
  for (const auto& input : *contract.inputs) {
    auto port = decode(*input.port);
    std::string bytes;
    if (!crypto::base64_decode(*input.bytes, bytes) || !types::verify(*port.type, bytes))
      throw std::runtime_error("invalid saved remote input value");
    std::optional<std::string> artifact;
    if (!input.artifact_id->empty()) artifact = *input.artifact_id;
    result.inputs.push_back({std::move(port.name), port.kind, std::move(port.type), std::move(bytes), std::move(artifact)});
  }
  for (const auto& source : *contract.sources)
    result.sources.push_back({*source.port, path_from_key(*source.card), *source.invocation});
  return result;
}

auto stage_remote_archive(const RemoteJobClient& client, const std::string& service, const std::string& server_id,
                          const std::string& instance, const stdfs::path& archive,
                          const std::atomic<bool>& cancel) -> std::string {
  crypto::Sha256 archive_hash;
  const auto archive_size = fs::stream_beneath(archive.parent_path(), archive.filename(), kMaxBlobBytes,
      [&](std::string_view chunk) {
        if (cancel.load()) throw Failed(FailureKind::cancelled, "remote Nix archive transfer cancelled");
        archive_hash.update(chunk);
      });
  const auto archive_digest = archive_hash.finish_hex();
  std::ifstream nar(archive, std::ios::binary);
  client.put_blob(service, server_id, instance, archive_digest, archive_size, nar, &cancel);
  return archive_digest;
}

auto stage_remote_invocation(const RemoteJobClient& client, const std::string& service, const std::string& server_id,
                             const std::string& instance, const std::string& project_id,
                             const stdfs::path& state, const std::string& archive_digest,
                             const ExecutionRequest& request) -> std::string {
  remote::InvocationJson invocation;
  *invocation.project_id = project_id;
  *invocation.network = request.node.settings.network;
  *invocation.filesystem = request.node.settings.filesystem;
  *invocation.implementation_index = request.build.implementation_index;
  auto& files = *invocation.files;
  auto& dirs = *invocation.directories;
  std::set<std::string> blobs;
  uint64_t total = 0;
  auto stage_file = [&](const stdfs::path& root, const stdfs::path& source, const stdfs::path& destination) {
    if (files.size() >= kMaxInputBlobs - 3) throw std::runtime_error("remote input has too many files");
    const auto path = remote::relative_path(destination.generic_string());
    const auto temporary = request.directory / (".input-" + crypto::uuid4());
    struct RemoveInput {
      stdfs::path path;
      ~RemoveInput() { std::error_code ec; stdfs::remove(path, ec); }
    } remove{temporary};
    std::ofstream snapshot(temporary, std::ios::binary);
    if (!snapshot) throw std::runtime_error("cannot stage remote input");
    crypto::Sha256 hash;
    const auto size = fs::stream_beneath(root, source, kMaxBlobBytes - total, [&](std::string_view chunk) {
      if (request.cancel.load()) throw Failed(FailureKind::cancelled, "run cancelled before external submission");
      hash.update(chunk);
      snapshot.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
      if (!snapshot) throw std::runtime_error("cannot stage remote input");
    });
    snapshot.close();
    if (!snapshot) throw std::runtime_error("cannot stage remote input");
    total += size;
    const auto digest = hash.finish_hex();
    if (blobs.insert(digest).second) {
      std::ifstream stream(temporary, std::ios::binary);
      client.put_blob(service, server_id, instance, digest, size, stream, &request.cancel);
    }
    remote::FileJson file;
    *file.path = path.generic_string();
    *file.sha256 = digest;
    *file.size = size;
    files.push_back(std::move(file));
  };
  std::vector<Value> inputs = request.inputs;
  for (size_t i = 0; i < inputs.size(); ++i) {
    auto& input = inputs[i];
    if (input.kind == PortKind::FlatBuffer) continue;
    const auto source = path_value(input);
    if (!source) throw std::runtime_error("remote file input has no path");
    const auto [root, rel] = input_location(state, *source);
    const auto destination = remote::relative_path((stdfs::path("inputs") / std::to_string(i) / rel.filename()).generic_string());
    if (input.kind == PortKind::File) {
      stage_file(root, rel, destination);
    } else {
      const auto listing = fs::list_beneath(root, rel, kMaxInputBlobs * 2);
      if (!listing.links.empty() || !listing.special.empty())
        throw std::runtime_error("remote Directory input contains a symlink or special file");
      dirs.push_back(destination.generic_string());
      for (const auto& child : listing.directories)
        dirs.push_back(remote::relative_path((destination / child).generic_string()).generic_string());
      for (const auto& name : listing.files) stage_file(root, rel / name, destination / name);
    }
    input = with_path(input, destination.generic_string());
  }
  if (request.cancel.load()) throw Failed(FailureKind::cancelled, "run cancelled before external submission");
  const auto ptype = NixRunner::params_type(request.node);
  const auto params = NixRunner::params_buffer(request.node);
  const std::vector<std::pair<std::string, std::string>> env{
      {"TSUGUMI_PARAMS_JSON", NixRunner::params_json(request.node, false)},
  };
  auto call = worker_call_frame(1, request.summary.run_id, request.node.path, request.summary.invocation, inputs,
      ptype->bfbs, ptype->root_type, params, request.node.outputs, "", "", "", env, {}, {});
  *invocation.call_sha256 = upload(client, service, server_id, instance, call, request.cancel);
  blobs.insert(*invocation.call_sha256);
  *invocation.archive_sha256 = archive_digest;
  blobs.insert(archive_digest);
  const auto invocation_digest = upload(client, service, server_id, instance, schema::encode(invocation), request.cancel);
  blobs.insert(invocation_digest);
  schema::v::Array<schema::v::String> blob_array;
  blob_array.value().assign(blobs.begin(), blobs.end());
  const auto input_blobs = schema::capture(schema::encode(blob_array));
  const auto invocation_sha256 = schema::string_value(invocation_digest);
  const auto& env_info = request.build.environment;
  const auto system = schema::string_value(env_info.system);
  const auto derivation = schema::string_value(env_info.derivation);
  const auto output = schema::string_value(env_info.manifest);
  const auto& settings = request.node.settings;
  const auto timeout = schema::capture(std::to_string(settings.timeout_seconds.value_or(3600)));
  std::vector<std::pair<std::string, const hosq::json::GenericDom<>*>> fields{
      {"system", &system}, {"derivation", &derivation}, {"output", &output},
      {"invocation_sha256", &invocation_sha256}, {"input_blobs", &input_blobs}, {"timeout_seconds", &timeout},
  };
  hosq::json::GenericDom<> cpu, memory;
  if (settings.execution_resources) {
    cpu = schema::capture(std::to_string(settings.execution_resources->cpu_millis));
    memory = schema::capture(std::to_string(settings.execution_resources->memory_bytes));
    fields.emplace_back("cpu_millis", &cpu);
    fields.emplace_back("memory_bytes", &memory);
  }
  return schema::object_text(fields);
}

auto import_remote_result(const RemoteJobClient& client, const std::string& service, const std::string& server_id,
                          const std::string& instance, const std::string& result_digest,
                          const std::string& spec, const stdfs::path& directory,
                          const std::vector<types::PortType>& outputs, const WorkerProc::LogSink& log,
                          const WorkerProc::DisplaySink& display, const std::atomic<bool>& stop) -> RemoteInvocationResult {
  remote::JobJson job;
  if (schema::decode(spec, job) || !job.has_required_fields()) throw std::runtime_error("invalid saved remote job spec");
  remote::ResultJson manifest;
  if (auto error = schema::decode(download(client, service, server_id, instance, result_digest, kMaxManifestBytes, stop), manifest))
    throw std::runtime_error("invalid remote result manifest: " + error->message);
  const auto* result_hash = schema::get(manifest.invocation_result_sha256);
  const auto* files = schema::get(manifest.outputs);
  if (!result_hash || !files || files->size() > 65'536)
    throw std::runtime_error("remote result manifest is missing fields or exceeds its entry limit");
  auto bytes = download(client, service, server_id, instance, *result_hash, kMaxWorkerBytes, stop);
  flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  if (!W::VerifyWorkerMessageBuffer(verifier)) throw std::runtime_error("invalid remote WorkerResult");
  const auto* message = W::GetWorkerMessage(bytes.data());
  RemoteInvocationResult result;
  result.environment.derivation = *schema::get(job.derivation);
  result.environment.manifest = *schema::get(job.output);
  result.environment.system = *schema::get(job.system);
  const auto temp = directory / (".remote-import-" + crypto::uuid4());
  stdfs::create_directories(temp);
  struct Remove {
    stdfs::path path;
    ~Remove() { std::error_code ec; stdfs::remove_all(path, ec); }
  } remove{temp};
  std::set<stdfs::path> written;
  uint64_t total = 0;
  for (const auto& file : *files) {
    const auto* name = schema::get(file.path);
    const auto* digest = schema::get(file.sha256);
    const auto* size = schema::get(file.size);
    if (!name || !digest || !size) throw std::runtime_error("remote result member is missing required fields");
    const auto rel = remote::relative_path(*name);
    const auto base = *rel.begin();
    if (base != "files" && rel != "environment.json" && rel != "logs.jsonl" && rel != "display.fb")
      throw std::runtime_error("unexpected remote result member");
    if (base == "files") {
      auto part = std::next(rel.begin());
      if (part == rel.end() || (*part != "out" && *part != "work" && *part != "tmp") || std::next(part) == rel.end())
        throw std::runtime_error("unexpected remote file member");
    }
    if (!crypto::is_sha256_hex(*digest) || *size > kMaxBlobBytes - total || !written.insert(rel).second)
      throw std::runtime_error("invalid remote result member");
    const auto max_size = base == "files" ? kMaxBlobBytes : kMaxWorkerBytes;
    if (*size > max_size) throw std::runtime_error("remote result member exceeds its size limit");
    const auto path = temp / rel;
    stdfs::create_directories(path.parent_path());
    std::ofstream destination(path, std::ios::binary | std::ios::trunc);
    if (!destination) throw std::runtime_error("cannot stage remote result");
    client.get_blob(service, server_id, instance, *digest, destination, *size, &stop);
    destination.close();
    if (!destination || stdfs::file_size(path) != *size) throw std::runtime_error("remote result size mismatch");
    total += *size;
  }
  remote::EnvironmentJson environment;
  const auto info = fs::read_file(temp / "environment.json");
  if (!info || schema::decode(*info, environment) || !std::ranges::all_of(*environment.paths, remote::store_path))
    throw std::runtime_error("invalid remote build environment");
  result.built = *environment.built;
  result.environment.paths = *environment.paths;
  if (!result.environment.paths.empty())
    result.environment.id = crypto::sha256_hex(result.environment.derivation + "\n" + result.environment.manifest + "\n" +
                                              result.environment.system);
  if (const auto* failure = message->value_as_WorkerFailure()) {
    result.worker.failure = wire::failure(failure->failure());
  } else if (const auto* worker = message->value_as_WorkerResult()) {
    std::set<std::string> seen;
    std::vector<std::pair<stdfs::path, PortKind>> returned_paths;
    for (const auto* output : *worker->outputs()) {
      const auto port = std::ranges::find(outputs, output->name()->str(), &types::PortType::name);
      if (port == outputs.end() || !seen.insert(port->name).second)
        throw Failed(FailureKind::output_invalid, "remote worker sent an unknown or duplicate port");
      std::string value(reinterpret_cast<const char*>(output->bytes()->data()), output->bytes()->size());
      if (!port->type || !types::verify(*port->type, value)) throw Failed(FailureKind::output_invalid, "invalid remote output value");
      if (port->kind != PortKind::FlatBuffer) {
        Value path{port->name, port->kind, port->type, std::move(value), {}};
        const auto relative = path_value(path);
        if (!relative) throw std::runtime_error("missing remote output path");
        const auto rel = remote::relative_path(*relative);
        if (*rel.begin() != "out") throw std::runtime_error("remote output is outside its output folder");
        returned_paths.emplace_back(rel, port->kind);
        value = with_path(path, (directory / rel).string()).bytes;
      }
      result.worker.outputs.emplace_back(port->name, std::move(value));
    }
    for (const auto& port : outputs)
      if (!port.optional && !seen.contains(port.name)) throw Failed(FailureKind::output_invalid, "remote worker omitted " + port.name);
    for (const auto& dir : *environment.directories) {
      const auto rel = remote::relative_path(dir);
      const auto top = *rel.begin();
      if (top != "out" && top != "work" && top != "tmp") throw std::runtime_error("invalid remote result directory");
      stdfs::create_directories(temp / "files" / rel);
    }
    for (const auto& [rel, kind] : returned_paths) {
      const auto status = stdfs::symlink_status(temp / "files" / rel);
      const bool exists = kind == PortKind::File ? stdfs::is_regular_file(status) : stdfs::is_directory(status);
      if (!exists) throw Failed(FailureKind::output_invalid, "remote output file or directory is missing or has the wrong kind");
    }
    for (const char* name : {"out", "work", "tmp"}) {
      const auto staged = temp / "files" / name;
      if (!stdfs::is_directory(staged)) continue;
      const auto target = directory / name;
      stdfs::remove_all(target);
      stdfs::rename(staged, target);
    }
  } else {
    throw std::runtime_error("remote worker did not send a result or failure");
  }
  log_result(temp / "logs.jsonl", log);
  if (display)
    if (auto blob = fs::read_file(temp / "display.fb"))
      if (auto decoded = wire::standalone_display(*blob)) display(*decoded);
  return result;
}

}  // namespace tsugumi
