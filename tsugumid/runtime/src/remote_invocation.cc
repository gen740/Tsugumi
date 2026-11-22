#include "remote_invocation.hh"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

#include "module_source.hh"
#include "tsugumi/protocol/worker_generated.h"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "wire.hh"

namespace tsugumi {
namespace stdfs = std::filesystem;
namespace W = Tsugumi::Worker;
namespace R = Tsugumi::RemoteWorker;

namespace {

constexpr uint64_t kMaxBlobBytes = uint64_t{64} << 30;
constexpr uint64_t kMaxManifestBytes = uint64_t{8} << 20;
constexpr uint64_t kMaxWorkerBytes = uint64_t{16} << 20;
constexpr size_t kMaxInputBlobs = 1024;

auto upload(const RemoteJobClient& client, const std::string& service,
            const std::string& instance, const std::string& job_id,
            const std::string& bytes, std::stop_token cancel,
            const TransferProgress& progress = {}) -> std::string {
  const std::string digest = crypto::sha256_hex(bytes);
  std::istringstream stream(bytes);
  client.put_blob(service, instance, job_id, digest, bytes.size(), stream,
                  cancel, progress);
  return digest;
}

auto download(const RemoteJobClient& client, const std::string& service,
              const std::string& instance, const std::string& job_id,
              const std::string& digest, uint64_t limit, std::stop_token stop,
              std::optional<uint64_t> expected_size = std::nullopt)
    -> std::string {
  if (!crypto::is_sha256_hex(digest))
    throw std::runtime_error("invalid remote blob digest");
  std::ostringstream bytes;
  client.get_blob(service, instance, job_id, digest, bytes, limit,
                  expected_size, stop);
  return bytes.str();
}

auto input_location(const std::vector<stdfs::path>& file_roots,
                    const std::string& path)
    -> std::pair<stdfs::path, stdfs::path> {
  for (const auto& root : file_roots) {
    auto rel = stdfs::path(path).lexically_normal().lexically_relative(root);
    if (!rel.empty() && rel != "." && *rel.begin() != "..") return {root, rel};
  }
  throw std::runtime_error("remote file input is outside the run's folders");
}

auto import_logs(const stdfs::path& path, const RemoteLogPrefix& recorded,
                 RemoteInvocationResult& result) -> void {
  std::ifstream stream(path, std::ios::binary);
  // The complete result is authoritative: the entries recorded while the job
  // ran must be exactly its prefix, so none is recorded twice or lost.
  crypto::Sha256 prefix;
  std::string buffer(64 << 10, '\0');
  for (uint64_t left = recorded.bytes; left > 0;) {
    const auto size =
        static_cast<std::streamsize>(std::min<uint64_t>(left, buffer.size()));
    stream.read(buffer.data(), size);
    if (stream.gcount() != size)
      throw std::runtime_error("remote log is shorter than its recorded part");
    prefix.update(buffer.data(), static_cast<size_t>(size));
    left -= static_cast<uint64_t>(size);
  }
  if (recorded.bytes && prefix.finish_hex() != recorded.sha256)
    throw std::runtime_error("remote log differs from its recorded part");
  std::string line;
  while (std::getline(stream, line))
    result.logs.push_back(decode_remote_log_entry(line));
}

}  // namespace

auto decode_remote_log_entry(std::string_view line)
    -> std::pair<LogStream, std::string> {
  remote::LogJson entry;
  // Decode the complete entry directly. A JSON DOM would own another copy
  // of the line and validate it before decoding the same bytes again.
  if (hosq::decode_document_into<hosq::json::FastReader<>>(
          entry, line, {.error_path = false}) ||
      *entry.stream <= static_cast<uint64_t>(LogStream::unspecified) ||
      *entry.stream > static_cast<uint64_t>(LogStream::MAX))
    throw std::runtime_error("invalid remote log entry");
  return {static_cast<LogStream>(*entry.stream), std::move(*entry.text)};
}

auto stage_remote_archive(const RemoteJobClient& client,
                          const std::string& service,
                          const std::string& instance,
                          const std::string& job_id, const stdfs::path& archive,
                          std::stop_token cancel,
                          const TransferProgress& progress) -> std::string {
  crypto::Sha256 archive_hash;
  const auto archive_size = fs::stream_beneath(
      archive.parent_path(), archive.filename(), kMaxBlobBytes,
      [&](std::string_view chunk) {
        if (cancel.stop_requested())
          throw Failed(FailureKind::cancelled,
                       "remote Nix archive transfer cancelled");
        archive_hash.update(chunk);
      });
  const auto archive_digest = archive_hash.finish_hex();
  std::ifstream nar(archive, std::ios::binary);
  client.put_blob(service, instance, job_id, archive_digest, archive_size, nar,
                  cancel, progress);
  return archive_digest;
}

auto stage_remote_invocation(const RemoteJobClient& client,
                             const std::string& service,
                             const std::string& instance,
                             const std::string& job_id,
                             const std::vector<stdfs::path>& file_roots,
                             const std::string& archive_digest,
                             const ExecutionRequest& request) -> std::string {
  remote::InvocationJson invocation;
  *invocation.implementation_index = request.build.implementation_index;
  auto& files = *invocation.files;
  auto& dirs = *invocation.directories;
  std::set<std::string> blobs;
  uint64_t total = 0;
  auto stage_file = [&](const stdfs::path& root, const stdfs::path& source,
                        const stdfs::path& destination) {
    if (files.size() >= kMaxInputBlobs - 3)
      throw std::runtime_error("remote input has too many files");
    const auto path = remote::relative_path(destination.generic_string());
    const auto temporary = request.directory / (".input-" + crypto::uuid4());
    struct RemoveInput {
      stdfs::path path;
      ~RemoveInput() {
        std::error_code ec;
        stdfs::remove(path, ec);
      }
    } remove{temporary};
    std::ofstream snapshot(temporary, std::ios::binary);
    if (!snapshot) throw std::runtime_error("cannot stage remote input");
    crypto::Sha256 hash;
    const auto size = fs::stream_beneath(
        root, source, kMaxBlobBytes - total, [&](std::string_view chunk) {
          if (request.stop.stop_requested())
            throw Failed(FailureKind::cancelled,
                         "run cancelled before external submission");
          hash.update(chunk);
          snapshot.write(chunk.data(),
                         static_cast<std::streamsize>(chunk.size()));
          if (!snapshot) throw std::runtime_error("cannot stage remote input");
        });
    snapshot.close();
    if (!snapshot) throw std::runtime_error("cannot stage remote input");
    total += size;
    const auto digest = hash.finish_hex();
    if (blobs.insert(digest).second) {
      std::ifstream stream(temporary, std::ios::binary);
      client.put_blob(service, instance, job_id, digest, size, stream,
                      request.stop, [&](uint64_t done, uint64_t bytes) {
                        if (request.transfer)
                          request.transfer("upload", path.generic_string(),
                                           done, bytes);
                      });
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
    const auto [root, rel] = input_location(file_roots, *source);
    const auto destination = remote::relative_path(
        (stdfs::path("inputs") / std::to_string(i) / rel.filename())
            .generic_string());
    if (input.kind == PortKind::File) {
      stage_file(root, rel, destination);
    } else {
      const auto listing = fs::list_beneath(root, rel, kMaxInputBlobs * 2);
      if (!listing.links.empty() || !listing.special.empty())
        throw std::runtime_error(
            "remote Directory input contains a symlink or special file");
      dirs.push_back(destination.generic_string());
      for (const auto& child : listing.directories)
        dirs.push_back(
            remote::relative_path((destination / child).generic_string())
                .generic_string());
      for (const auto& name : listing.files)
        stage_file(root, rel / name, destination / name);
    }
    input = with_path(input, destination.generic_string());
  }
  if (request.stop.stop_requested())
    throw Failed(FailureKind::cancelled,
                 "run cancelled before external submission");
  const auto ptype = NixRunner::params_type(request.node);
  const auto params = NixRunner::params_buffer(request.node);
  // The remote adapter uses this JSON for Bash implementations selected
  // from the realized manifest. Native SDKs read the parameter buffer.
  const std::vector<std::pair<std::string, std::string>> env{
      {"TSUGUMI_PARAMS_JSON", NixRunner::params_json(request.node, false)},
  };
  auto call = worker_call_frame(
      1, request.summary.run_id, request.node.path, request.summary.invocation,
      inputs, ptype->bfbs, ptype->root_type, params, request.node.outputs, "",
      "", "", env,
      request.node.settings.timeout_seconds
          ? std::optional<uint64_t>(*request.node.settings.timeout_seconds *
                                    1000)
          : std::nullopt);
  *invocation.call_sha256 =
      upload(client, service, instance, job_id, call, request.stop);
  blobs.insert(*invocation.call_sha256);
  *invocation.archive_sha256 = archive_digest;
  blobs.insert(archive_digest);
  const auto invocation_digest =
      upload(client, service, instance, job_id, schema::encode(invocation),
             request.stop);
  blobs.insert(invocation_digest);
  R::JobSpecT job;
  job.input_blobs.assign(blobs.begin(), blobs.end());
  job.invocation_sha256 = invocation_digest;
  job.system = request.build.derivation.system;
  job.derivation = request.build.derivation.path;
  job.output = request.build.derivation.output;
  job.log_limit_bytes = request.log_limit_bytes;
  // Names only: the values follow in a JobSecretsRequest once the worker
  // waits for them.
  job.secrets = request.node.settings.secrets;
  if (request.node.settings.build_timeout_seconds) {
    const auto budget = *request.node.settings.build_timeout_seconds * 1000;
    if (request.build.preparation_ms >= budget)
      throw Failed(FailureKind::timeout, "Nix exceeded buildTimeoutSeconds");
    job.build_timeout_ms = budget - request.build.preparation_ms;
  }
  const auto& settings = request.node.settings;
  job.cpu_millis = settings.sandbox.limits.cpu_millis;
  job.memory_bytes = settings.sandbox.limits.memory_bytes;
  job.filesystem = settings.sandbox.filesystem;
  job.pid = settings.sandbox.pid;
  job.ipc = settings.sandbox.ipc;
  job.identity = settings.sandbox.identity;
  job.network = settings.grants.network;
  flatbuffers::FlatBufferBuilder b;
  b.Finish(R::JobSpec::Pack(b, &job));
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}

auto import_remote_result(
    const RemoteJobClient& client, const std::string& service,
    const std::string& instance, const std::string& job_id,
    const std::string& result_digest, const std::string& spec,
    const stdfs::path& directory, const stdfs::path& module_cache,
    const std::vector<types::PortType>& outputs,
    const WorkerProc::DisplaySink& display, const RemoteLogPrefix& recorded,
    std::stop_token stop,
    const std::function<void(std::string_view, uint64_t, uint64_t)>& progress,
    const std::function<void(int64_t, int64_t)>& on_validation)
    -> RemoteInvocationResult {
  const auto job = workerd::parse_spec(spec);
  const auto manifest_bytes = download(client, service, instance, job_id,
                                       result_digest, kMaxManifestBytes, stop);
  flatbuffers::Verifier manifest_verifier(
      reinterpret_cast<const uint8_t*>(manifest_bytes.data()),
      manifest_bytes.size());
  if (!manifest_verifier.VerifyBuffer<R::ResultManifest>(nullptr))
    throw std::runtime_error("invalid remote result manifest");
  const auto* manifest =
      flatbuffers::GetRoot<R::ResultManifest>(manifest_bytes.data());
  const auto result_hash = manifest->invocation_result_sha256()->str();
  const auto* files = manifest->outputs();
  if (files->size() > 65536)
    throw std::runtime_error("too many remote result members");
  auto bytes = download(client, service, instance, job_id, result_hash,
                        kMaxWorkerBytes, stop);
  flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(bytes.data()),
                                 bytes.size());
  if (!W::VerifyWorkerMessageBuffer(verifier))
    throw std::runtime_error("invalid remote WorkerResult");
  const auto* message = W::GetWorkerMessage(bytes.data());
  RemoteInvocationResult result;
  result.environment.derivation = job->derivation;
  result.environment.manifest = job->output;
  result.environment.system = job->system;
  const auto temp = directory / (".remote-import-" + crypto::uuid4());
  stdfs::create_directories(temp);
  struct Remove {
    stdfs::path path;
    ~Remove() {
      std::error_code ec;
      stdfs::remove_all(path, ec);
    }
  } remove{temp};
  std::set<stdfs::path> written;
  uint64_t total = 0;
  std::vector<std::pair<ModuleIdentity, const R::ResultFile*>> module_files;
  for (const auto* file : *files) {
    const auto name_value = file->path()->str();
    const auto digest_value = file->sha256()->str();
    const auto size_value = file->size();
    const auto* name = &name_value;
    const auto* digest = &digest_value;
    const auto* size = &size_value;
    const auto rel = remote::relative_path(*name);
    const auto base = *rel.begin();
    if (base != "files" && base != "modules" && rel != "environment.json" &&
        rel != "logs.jsonl" && rel != "display.fb")
      throw std::runtime_error("unexpected remote result member");
    if (base == "files") {
      auto part = std::next(rel.begin());
      if (part == rel.end() ||
          (*part != "out" && *part != "work" && *part != "tmp") ||
          std::next(part) == rel.end())
        throw std::runtime_error("unexpected remote file member");
    }
    if (!crypto::is_sha256_hex(*digest) || *size > kMaxBlobBytes - total ||
        !written.insert(rel).second)
      throw std::runtime_error("invalid remote result member");
    const auto max_size = base == "modules"     ? kModuleSourceLimit
                          : base == "files"     ? kMaxBlobBytes
                          : rel == "logs.jsonl" ? job->log_limit_bytes
                                                : kMaxWorkerBytes;
    if (*size > max_size)
      throw std::runtime_error("remote result member exceeds its size limit");
    total += *size;
    if (base == "modules") {
      const auto name = rel.parent_path().filename().string();
      if (rel == stdfs::path("modules") / name / "index.js")
        if (auto identity = ModuleIdentity::parse("/nix/store/" + name))
          module_files.emplace_back(std::move(*identity), file);
      continue;  // Built definition resources are independent of display data.
    }
    const auto path = temp / rel;
    stdfs::create_directories(path.parent_path());
    std::ofstream destination(path, std::ios::binary | std::ios::trunc);
    if (!destination) throw std::runtime_error("cannot stage remote result");
    client.get_blob(service, instance, job_id, *digest, destination, *size,
                    *size, stop, [&](uint64_t done, uint64_t bytes) {
                      if (progress) progress(*name, done, bytes);
                    });
    destination.close();
    if (!destination) throw std::runtime_error("cannot close remote result");
  }
  remote::EnvironmentJson environment;
  const auto info = fs::read_file(temp / "environment.json");
  if (!info || schema::decode(*info, environment))
    throw std::runtime_error("invalid remote build environment");
  result.built = *environment.built;
  result.log_truncated = *environment.log_truncated;
  result.environment.paths = *environment.paths;
  std::optional<DisplayData> decoded_display;
  if (auto blob = fs::read_file(temp / "display.fb"))
    decoded_display = wire::standalone_display(*blob);
  for (const auto& [identity, file] : module_files) {
    if (std::ranges::find(result.environment.paths, identity.path().string()) ==
        result.environment.paths.end())
      continue;
    try {
      const auto& root = identity.path();
      if (stdfs::is_symlink(root)) continue;
      if (!stdfs::is_directory(root) &&
          !cached_module_source(module_cache, identity)) {
        const auto source =
            download(client, service, instance, job_id, file->sha256()->str(),
                     kModuleSourceLimit, stop, file->size());
        // The pinned builder supplies built resources, restricted to its
        // closure. A Nix store path is an identity, not the content hash of
        // this source.
        save_module_source(module_cache, identity, source);
      }
    } catch (const std::exception&) {
      if (stop.stop_requested()) throw;
      // The clients show unavailable presentation; computation still completes.
    }
  }
  if (const auto* failure = message->value_as_WorkerFailure()) {
    result.failure = wire::failure(failure->failure());
  } else if (const auto* worker = message->value_as_WorkerResult()) {
    const auto started = now_ms();
    std::vector<std::pair<std::string, std::string>> raw;
    for (const auto* output : *worker->outputs())
      raw.emplace_back(
          output->name()->str(),
          std::string(reinterpret_cast<const char*>(output->bytes()->data()),
                      output->bytes()->size()));
    result.outputs = workerd::accept_outputs(std::move(raw), outputs);
    std::vector<std::pair<stdfs::path, PortKind>> returned_paths;
    for (auto& value : result.outputs) {
      if (value.kind == PortKind::FlatBuffer) continue;
      const auto relative = path_value(value);
      if (!relative) throw std::runtime_error("missing remote output path");
      const auto rel = remote::relative_path(*relative);
      if (*rel.begin() != "out")
        throw std::runtime_error("remote output is outside its output folder");
      returned_paths.emplace_back(rel, value.kind);
      value = with_path(value, (directory / rel).string());
    }
    for (const auto& dir : *environment.directories) {
      const auto rel = remote::relative_path(dir);
      const auto top = *rel.begin();
      if (top != "out" && top != "work" && top != "tmp")
        throw std::runtime_error("invalid remote result directory");
      stdfs::create_directories(temp / "files" / rel);
    }
    for (const auto& [rel, kind] : returned_paths) {
      const auto status = stdfs::symlink_status(temp / "files" / rel);
      const bool exists = kind == PortKind::File
                              ? stdfs::is_regular_file(status)
                              : stdfs::is_directory(status);
      if (!exists)
        throw Failed(
            FailureKind::output_invalid,
            "remote output file or directory is missing or has the wrong kind");
    }
    if (stop.stop_requested())
      throw BrokerDisconnected("remote result import stopped");
    for (const char* name : {"out", "work", "tmp"}) {
      const auto staged = temp / "files" / name;
      if (!stdfs::is_directory(staged)) continue;
      const auto target = directory / name;
      stdfs::remove_all(target);
      stdfs::rename(staged, target);
    }
    if (on_validation) on_validation(started, now_ms());
  } else {
    throw std::runtime_error("remote worker did not send a result or failure");
  }
  import_logs(temp / "logs.jsonl", recorded, result);
  if (display && decoded_display) display(*decoded_display);
  return result;
}

}  // namespace tsugumi
