#include "remote_invocation.hh"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "tsugumi/protocol/worker_generated.h"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/log.hh"
#include "tsugumi/workerd/artifact_id.hh"
#include "tsugumi/workerd/frame.hh"
#include "tsugumi/workerd/log.hh"
#include "tsugumi/workerd/staging.hh"
#include "wire.hh"

namespace tsugumi {
namespace stdfs = std::filesystem;
namespace W = Tsugumi::Worker;
namespace R = Tsugumi::RemoteWorker;

namespace {

constexpr uint64_t kMaxBlobBytes = uint64_t{64} << 30;
constexpr uint64_t kMaxManifestBytes = uint64_t{8} << 20;
constexpr uint64_t kMaxWorkerBytes = workerd::kMaxWorkerFrameBytes;
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

auto deliver_remote_artifact(const RemoteJobClient& client,
                             const std::string& service,
                             const std::string& instance,
                             const std::string& job_id,
                             const std::string& fingerprint,
                             const std::string& uuid, const stdfs::path& source,
                             std::stop_token stop) -> void {
  R::JobArtifactRequestT answer;
  answer.fingerprint = fingerprint;
  answer.id = uuid;
  if (!source.empty()) {
    try {
      const auto tree = fs::list_beneath(source, "", kMaxInputBlobs * 2);
      if (tree.files.size() >= kMaxInputBlobs || !tree.links.empty() ||
          !tree.special.empty())
        throw std::runtime_error("cannot stage remote artifact tree");
      uint64_t total = 0;
      std::set<std::string> uploaded;
      answer.directories = tree.directories;
      for (const auto& name : tree.files) {
        if (stop.stop_requested())
          throw BrokerDisconnected("remote artifact transfer cancelled");
        crypto::Sha256 hash;
        const auto size = fs::stream_beneath(
            source, name, kMaxBlobBytes - total, [&](std::string_view chunk) {
              if (stop.stop_requested())
                throw BrokerDisconnected("remote artifact transfer cancelled");
              hash.update(chunk);
            });
        const auto digest = hash.finish_hex();
        total += size;
        // Artifacts are sealed. The worker checks the digest of the uploaded
        // stream before it publishes the blob.
        if (uploaded.insert(digest).second) {
          std::ifstream contents(source / name, std::ios::binary);
          if (!contents)
            throw std::runtime_error("cannot open artifact member");
          client.put_blob(service, instance, job_id, digest, size, contents,
                          stop);
        }
        auto file = std::make_unique<R::InvocationFileT>();
        file->path = name;
        file->sha256 = digest;
        file->size = size;
        answer.files.push_back(std::move(file));
      }
      answer.found = true;
    } catch (const BrokerDisconnected&) {
      throw;
    } catch (const BrokerInstanceChanged&) {
      throw;
    } catch (const std::exception&) {
      answer.files.clear();
      answer.directories.clear();
    }
  }
  client.deliver_artifact(service, instance, job_id, std::move(answer), stop);
}

auto stage_remote_invocation(const RemoteJobClient& client,
                             const std::string& service,
                             const std::string& instance,
                             const std::string& job_id,
                             const std::string& archive_digest,
                             const ExecutionRequest& request) -> std::string {
  R::InvocationT invocation;
  invocation.implementation_index = request.build.implementation_index;
  std::set<std::string> blobs;
  if (request.stop.stop_requested())
    throw Failed(FailureKind::cancelled,
                 "run cancelled before external submission");
  auto call =
      worker_call_frame(1, request.inputs, request.node.outputs,
                        request.node.settings.timeout_seconds
                            ? std::optional<uint64_t>(
                                  *request.node.settings.timeout_seconds * 1000)
                            : std::nullopt,
                        {});
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
  job.card_probe = request.build.card_probe;
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
    const stdfs::path& state_directory, const stdfs::path& artifact_destination,
    const std::vector<types::PortType>& outputs,
    const RemoteLogPrefix& recorded, const WorkerProc::LogSink& log,
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
  const auto temp = state_directory / (".remote-import-" + crypto::uuid4());
  stdfs::create_directories(temp);
  struct Remove {
    stdfs::path path;
    ~Remove() {
      std::error_code ec;
      stdfs::remove_all(path, ec);
    }
  } remove{temp};
  std::set<stdfs::path> written;
  const auto* failure = message->value_as_WorkerFailure();
  const auto* worker = message->value_as_WorkerResult();
  if (!failure && !worker)
    throw std::runtime_error("remote worker did not send a result or failure");
  const auto started = now_ms();
  std::set<stdfs::path> declared;
  std::set<ArtifactKey> ids;
  const auto* artifacts = failure ? failure->created() : worker->created();
  if (artifacts && artifacts->size() != 0 &&
      (job->card_probe || artifact_destination.empty()))
    throw Failed(FailureKind::output_invalid,
                 "a remote probe cannot publish artifacts");
  if (artifacts) {
    for (const auto* artifact : *artifacts) {
      auto id = wire::artifact_id(*artifact->id());
      if ((id[6] & 0xf0) != 0x40 || (id[8] & 0xc0) != 0x80 ||
          !ids.insert(id).second)
        throw Failed(FailureKind::output_invalid,
                     "invalid remote artifact UUID");
      declared.insert(std::to_string(result.created.size()));
      result.created.push_back({std::move(id), artifact->keep()});
    }
  }
  const auto permitted = [&](const stdfs::path& relative, bool allow_root) {
    auto part = relative.begin();
    if (part == relative.end() || *part++ != "artifacts") return false;
    return part == relative.end() ? allow_root : declared.contains(*part);
  };
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
    const bool output_file = *rel.begin() == "files" &&
        permitted(rel.lexically_relative("files"), false);
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
    const auto max_size = output_file ? kMaxBlobBytes
                          : name == "logs"
                              ? (job->card_probe ? kProbeTransportBytes
                                                 : kRemoteLogTransportBytes)
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
  std::vector<RawOutput> raw;
  if (failure) {
    result.failure = wire::failure(failure->failure());
  } else {
    for (const auto* output : *worker->outputs()) {
      const auto* bytes = output->bytes();
      raw.push_back({output->name()->str(),
                     bytes ? std::string(bytes->begin(), bytes->end()) : ""});
    }
  }
  result.outputs = workerd::accept_outputs(
      std::move(raw), failure ? std::span<const types::PortType>{} : outputs);
  std::set<stdfs::path> directories;
  for (const auto& dir : environment->directories) {
    const auto rel = remote::relative_path(dir);
    if (!permitted(rel, true) || !directories.insert(rel).second)
      throw std::runtime_error("invalid remote artifact directory");
    if (rel != "artifacts") count_entry();
    stdfs::create_directories(temp / "files" / rel);
  }
  const auto staged = temp / "files" / "artifacts";
  for (const auto& id : declared)
    if (!stdfs::is_directory(staged / id))
      throw Failed(FailureKind::output_invalid, "remote artifact directory is missing");
  import_logs(temp / "logs", recorded, log, result);
  if (stop.stop_requested())
    throw BrokerDisconnected("remote result import stopped");
  if (declared.empty()) {
    if (on_validation) on_validation(started, now_ms());
  } else {
    const int fd =
        ::open(staged.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open downloaded artifacts");
    struct Close {
      int fd;
      ~Close() { ::close(fd); }
    } close{fd};
    fs::OutputBudget budget{job->output_limit_bytes, job->output_limit_entries};
    auto moved = declared.begin();
    try {
      for (; moved != declared.end(); ++moved)
        fs::move_staging_artifact(fd, *moved, artifact_destination / *moved,
                                  budget);
      if (on_validation) on_validation(started, now_ms());
    } catch (...) {
      // Only this import's unpublished moves belong to the rollback.
      for (auto it = declared.begin(); it != moved; ++it) {
        const auto path = artifact_destination / *it;
        try {
          fs::clear_staging(path);
          stdfs::remove(path);
        } catch (const std::exception& error) {
          tsugumi::log::warn("rollback remote artifact: " +
                             std::string(error.what()));
        }
      }
      throw;
    }
  }
  return result;
}

}  // namespace tsugumi
