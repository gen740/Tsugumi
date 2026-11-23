#include "remote_invocation.hh"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "tsugumi/protocol/worker_generated.h"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/workerd/log.hh"
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

auto staged_input_path(size_t index, const stdfs::path& name) -> stdfs::path {
  return remote::relative_path(
      (stdfs::path("inputs") / std::to_string(index) / name.filename())
          .generic_string());
}

auto import_logs(const stdfs::path& path, const RemoteLogPrefix& recorded,
                 const WorkerProc::LogSink& sink, RemoteInvocationResult& result) -> void {
  const auto bytes = fs::read_file(path).value_or("");
  // The complete result is authoritative: the entries recorded while the job
  // ran must be exactly its prefix, so none is recorded twice or lost.
  if (bytes.size() < recorded.bytes)
    throw std::runtime_error("remote log is shorter than its recorded part");
  if (recorded.bytes &&
      crypto::sha256_hex(std::string_view(bytes).substr(0, recorded.bytes)) !=
          recorded.sha256)
    throw std::runtime_error("remote log differs from its recorded part");
  workerd::InvocationLog auxiliary_budget(kInvocationLogLimitBytes);
  const auto rest = std::string_view(bytes).substr(recorded.bytes);
  const auto used = remote::decode_log_entries(
      rest, [&](LogStream kind, std::string text, std::string_view) {
        if (kind == LogStream::standard_output ||
            kind == LogStream::standard_error) {
          if (sink) sink(kind, text);
        } else if (auxiliary_budget.append(kind, text)) {
          result.auxiliary_logs.emplace_back(kind, std::move(text));
        }
      });
  if (used != rest.size())
    throw std::runtime_error("remote log ends inside an entry");
  result.auxiliary_logs_truncated = auxiliary_budget.truncated();
}

}  // namespace

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
  remote::RW::InvocationT invocation;
  invocation.implementation_index = request.build.implementation_index;
  auto& files = invocation.files;
  auto& dirs = invocation.directories;
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
    auto file = std::make_unique<remote::RW::InvocationFileT>();
    file->path = path.generic_string();
    file->sha256 = digest;
    file->size = size;
    files.push_back(std::move(file));
  };
  std::vector<Value> inputs = request.inputs;
  for (size_t i = 0; i < inputs.size(); ++i) {
    auto& input = inputs[i];
    if (input.kind == PortKind::FlatBuffer) continue;
    if (input.path.empty())
      throw std::runtime_error("remote Path input has no path");
    const auto [root, rel] = input_location(file_roots, input.path);
    const auto destination = staged_input_path(i, rel);
    if (!fs::directory_beneath(root, rel)) {
      stage_file(root, rel, destination);
    } else {
      const auto listing = fs::list_beneath(root, rel, kMaxInputBlobs * 2);
      if (!listing.links.empty() || !listing.special.empty())
        throw std::runtime_error(
            "remote directory input contains a symlink or special file");
      dirs.push_back(destination.generic_string());
      for (const auto& child : listing.directories)
        dirs.push_back(
            remote::relative_path((destination / child).generic_string())
                .generic_string());
      for (const auto& name : listing.files)
        stage_file(root, rel / name, destination / name);
    }
    input.path = destination.generic_string();
  }
  if (request.stop.stop_requested())
    throw Failed(FailureKind::cancelled,
                 "run cancelled before external submission");
  auto call = worker_call_frame(
      1, inputs, {}, request.node.outputs,
      request.node.settings.timeout_seconds
          ? std::optional<uint64_t>(*request.node.settings.timeout_seconds *
                                    1000)
          : std::nullopt);
  invocation.call_sha256 =
      upload(client, service, instance, job_id, call, request.stop);
  blobs.insert(invocation.call_sha256);
  invocation.archive_sha256 = archive_digest;
  blobs.insert(archive_digest);
  const auto invocation_digest = upload(
      client, service, instance, job_id,
      remote::encode<remote::RW::Invocation>(invocation), request.stop);
  blobs.insert(invocation_digest);
  R::JobSpecT job;
  job.input_blobs.assign(blobs.begin(), blobs.end());
  job.invocation_sha256 = invocation_digest;
  job.system = request.build.derivation.system;
  job.derivation = request.build.derivation.path;
  job.output = request.build.derivation.output;
  job.log_limit_bytes = request.log_limit_bytes;
  job.output_limit_bytes = request.node.settings.output_limit_bytes;
  job.output_limit_entries = request.node.settings.output_limit_entries;
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
    const stdfs::path& directory,
    const std::vector<types::PortType>& outputs,
    const std::vector<Value>& inputs,
    const RemoteLogPrefix& recorded,
    const WorkerProc::LogSink& log,
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
  auto output_bytes = job->output_limit_bytes;
  auto output_entries = job->output_limit_entries;
  const auto count_entry = [&] {
    if (!output_entries)
      throw Failed(FailureKind::output_invalid,
                   "remote outputs exceed the entry limit");
    --output_entries;
  };
  for (const auto* file : *files) {
    const auto name = file->path()->str();
    const auto digest = file->sha256()->str();
    const auto size = file->size();
    const auto rel = remote::relative_path(name);
    const bool output_file = name.starts_with("files/out/");
    if (!output_file && name != "environment" && name != "logs")
      throw std::runtime_error("unexpected remote result member");
    if (output_file) {
      count_entry();
      if (size > output_bytes)
        throw Failed(FailureKind::output_invalid,
                     "remote outputs exceed the byte limit");
      output_bytes -= size;
    }
    if (!crypto::is_sha256_hex(digest) || !written.insert(rel).second)
      throw std::runtime_error("invalid remote result member");
    const auto max_size = output_file            ? kMaxBlobBytes
                          : name == "logs" ? kRemoteLogTransportBytes
                                                 : kMaxWorkerBytes;
    if (size > max_size)
      throw std::runtime_error("remote result member exceeds its size limit");
    const auto path = temp / rel;
    stdfs::create_directories(path.parent_path());
    std::ofstream destination(path, std::ios::binary | std::ios::trunc);
    if (!destination) throw std::runtime_error("cannot stage remote result");
    client.get_blob(service, instance, job_id, digest, destination, size, size,
                    stop, [&](uint64_t done, uint64_t bytes) {
                      if (progress) progress(name, done, bytes);
                    });
    destination.close();
    if (!destination) throw std::runtime_error("cannot close remote result");
  }
  const auto info = fs::read_file(temp / "environment");
  const auto environment =
      info ? remote::decode<remote::RW::ResultEnvironment>(*info) : std::nullopt;
  if (!environment) throw std::runtime_error("invalid remote build environment");
  result.built = environment->built;
  result.environment.paths = environment->paths;
  if (const auto* failure = message->value_as_WorkerFailure()) {
    result.failure = wire::failure(failure->failure());
  } else if (const auto* worker = message->value_as_WorkerResult()) {
    const auto started = now_ms();
    // Paths are job-relative: out/... for a new output, or the staged path of
    // the input that forwarded_from names, returned unchanged.
    std::vector<RawOutput> raw;
    const auto text = [](const flatbuffers::String* value) {
      return value ? value->str() : std::string();
    };
    for (const auto* output : *worker->outputs()) {
      const auto* bytes = output->bytes();
      raw.push_back(
          {output->name()->str(),
           bytes ? std::string(reinterpret_cast<const char*>(bytes->data()),
                               bytes->size())
                 : std::string(),
           text(output->path()), text(output->forwarded_from())});
    }
    std::map<std::string, std::string> forwards;
    for (const auto& output : raw)
      if (!output.forwarded_from.empty())
        forwards.emplace(output.name, output.forwarded_from);
    result.outputs = workerd::accept_outputs(std::move(raw), outputs);
    std::vector<stdfs::path> returned_paths;
    for (auto& value : result.outputs) {
      const auto forward = forwards.find(value.name);
      if (value.kind == PortKind::FlatBuffer && forward == forwards.end())
        continue;
      const auto invalid = [&](std::string message) {
        return Failed(FailureKind::output_invalid, std::move(message),
                      {{"port", value.name}});
      };
      if (value.kind == PortKind::FlatBuffer)
        throw invalid("a FlatBuffer output cannot forward a path");
      if (value.path.empty()) throw invalid("missing remote output path");
      const auto rel = remote::relative_path(value.path);
      if (forward != forwards.end()) {
        const auto input =
            std::ranges::find(inputs, forward->second, &Value::name);
        if (input == inputs.end() || input->kind != PortKind::Path)
          throw invalid("remote output forwards an unknown input");
        if (input->path.empty() ||
            rel != staged_input_path(input - inputs.begin(), input->path))
          throw invalid("remote output does not match the forwarded input");
        value.path = input->path;
        continue;
      }
      if (*rel.begin() != "out")
        throw invalid("remote output is outside its output folder");
      returned_paths.push_back(rel);
      value.path = (directory / rel).string();
    }
    std::set<stdfs::path> directories;
    for (const auto& dir : environment->directories) {
      const auto rel = remote::relative_path(dir);
      if (*rel.begin() != "out" || !directories.insert(rel).second)
        throw std::runtime_error("invalid remote result directory");
      if (rel != "out" ||
          std::ranges::find(returned_paths, rel) != returned_paths.end())
        count_entry();
      stdfs::create_directories(temp / "files" / rel);
    }
    for (const auto& rel : returned_paths) {
      const auto status = stdfs::symlink_status(temp / "files" / rel);
      if (!stdfs::is_regular_file(status) && !stdfs::is_directory(status))
        throw Failed(FailureKind::output_invalid,
                     "remote output is missing or is neither a regular file "
                     "nor a directory");
    }
    if (stop.stop_requested())
      throw BrokerDisconnected("remote result import stopped");
    const auto staged = temp / "files" / "out";
    if (stdfs::is_directory(staged)) {
      const auto target = directory / "out";
      stdfs::remove_all(target);
      stdfs::rename(staged, target);
    }
    if (on_validation) on_validation(started, now_ms());
  } else {
    throw std::runtime_error("remote worker did not send a result or failure");
  }
  import_logs(temp / "logs", recorded, log, result);
  return result;
}

}  // namespace tsugumi
