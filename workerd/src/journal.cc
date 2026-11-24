#include "journal.hh"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/workerd/invocation.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi::workerd {
namespace {
struct Fd {
  int value;
  ~Fd() {
    if (value >= 0) ::close(value);
  }
};
auto durable_directory(const stdfs::path& path) -> void {
  Fd fd{::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (fd.value < 0 || ::fsync(fd.value) != 0)
    throw std::runtime_error("cannot sync journal directory");
}
auto bounded_file(const stdfs::path& path, uint64_t limit) -> std::string {
  if (stdfs::file_size(path) > limit)
    throw Error(R::ErrorCode::Invalid, "file exceeds size limit");
  return fs::read_file_or_throw(path);
}
auto require_owner(const Record& state, std::string_view core) -> void {
  if (!state.core_life_id.empty() && state.core_life_id != core)
    throw Error(R::ErrorCode::Rejected, "job belongs to another core life");
}
}  // namespace

auto Record::status() const -> R::JobStatusT {
  R::JobStatusT s;
  s.state = state;
  s.fingerprint = fingerprint;
  s.result_sha256 = result;
  s.diagnostic = diagnostic;
  return s;
}
auto now_ms() -> uint64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

auto private_directory(const stdfs::path& path) -> void {
  if (!path.is_absolute())
    throw std::runtime_error("state directory must be absolute");
  stdfs::create_directories(path);
  struct stat s{};
  if (::lstat(path.c_str(), &s) != 0 || !S_ISDIR(s.st_mode) ||
      s.st_uid != ::geteuid() || (s.st_mode & 0077))
    throw std::runtime_error(
        "state directory must be private and owned by this user");
}
auto identity(const stdfs::path& directory, const std::string& name)
    -> std::string {
  const auto path = directory / name;
  if (!stdfs::exists(path))
    fs::write_atomic(path, crypto::random_token(32), 0600);
  auto value = bounded_file(path, 64);
  return value;
}
auto discard_tree(const stdfs::path& path) -> void {
  if (!stdfs::exists(stdfs::symlink_status(path))) return;
  if (stdfs::is_directory(stdfs::symlink_status(path))) {
    stdfs::permissions(path, stdfs::perms::owner_all, stdfs::perm_options::add);
    for (const auto& entry : stdfs::directory_iterator(path))
      discard_tree(entry.path());
    stdfs::remove(path);
  } else {
    stdfs::remove(path);
  }
}
auto file_digest(const stdfs::path& path, uint64_t limit) -> std::string {
  crypto::Sha256 hash;
  fs::stream_beneath(path.parent_path(), path.filename(), limit,
                     [&](std::string_view data) { hash.update(data); });
  return hash.finish_hex();
}
Journal::Journal(stdfs::path root) : root_(std::move(root)) {
  private_directory(root_);
  private_directory(root_ / "jobs");
  private_directory(root_ / "locks");
  instance_ = identity(root_, "instance_id");
}
auto Journal::job_path(std::string_view job) const -> stdfs::path {
  return root_ / "jobs" / job;
}
auto Journal::supervisor_lock_path(std::string_view job) const -> stdfs::path {
  return root_ / "locks" / (std::string(job) + ".supervisor");
}
auto Journal::locked(
    std::string_view job,
    const std::function<void(const stdfs::path&)>& action) const -> void {
  Fd fd{::open((root_ / "locks" / job).c_str(),
               O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)};
  if (fd.value < 0 || ::flock(fd.value, LOCK_EX) != 0)
    throw std::runtime_error("cannot lock journal");
  action(job_path(job));
}
auto Journal::jobs() const -> std::vector<std::string> {
  std::vector<std::string> result;
  for (const auto& entry : stdfs::directory_iterator(root_ / "jobs"))
    if (crypto::is_sha256_hex(entry.path().filename().string()))
      result.push_back(entry.path().filename().string());
  return result;
}
auto Journal::read(const stdfs::path& directory) const -> Record {
  if (!stdfs::exists(directory / "state")) return {};
  const auto s = remote::decode<R::JournalState>(
      bounded_file(directory / "state", 16384));
  if (!s || s->state > R::JobState::Released)
    throw std::runtime_error("corrupt job journal");
  return {s->state,       s->fingerprint, s->result, s->diagnostic,
          s->pid,         s->start,       s->boot,   s->core_life_id};
}
auto Journal::write(const stdfs::path& directory, const Record& r) const
    -> void {
  private_directory(directory);
  durable_directory(directory.parent_path());
  R::JournalStateT s;
  s.state = r.state;
  s.fingerprint = r.fingerprint;
  s.result = r.result;
  s.diagnostic = r.diagnostic.substr(0, 4096);
  s.pid = r.pid;
  s.start = r.start;
  s.boot = r.boot;
  s.core_life_id = r.core_life_id;
  fs::write_atomic(directory / "state", remote::encode<R::JournalState>(s),
                   0600);
}
auto Journal::status(std::string_view job,
                     std::optional<std::string_view> core) const -> Record {
  Record result;
  locked(job, [&](const auto& directory) {
    result = read(directory);
    if (core) require_owner(result, *core);
  });
  return result;
}
auto Journal::spec(std::string_view job) const -> std::string {
  return bounded_file(job_path(job) / "spec.fb", kMaxSpec);
}

auto Journal::put(std::string_view job, const R::PutBlobRequestT& c,
                  std::string_view core) -> void {
  if (c.total > kMaxBlob || c.bytes.size() > kChunkBytes ||
      c.offset > c.total || c.bytes.size() > c.total - c.offset ||
      (c.bytes.empty() && c.total != 0))
    throw Error(R::ErrorCode::Invalid, "invalid blob chunk bounds");
  locked(job, [&](const auto& dir) {
    auto state = read(dir);
    require_owner(state, core);
    if (state.state != R::JobState::Missing &&
        state.state != R::JobState::Uploading &&
        !(state.state == R::JobState::Running &&
          stdfs::exists(stdfs::symlink_status(dir / "work/artifact-wanted")) &&
          !stdfs::exists(stdfs::symlink_status(dir / "work/artifact-reply"))))
      throw Error(R::ErrorCode::Rejected, "job no longer accepts uploads");
    if (state.state == R::JobState::Missing) {
      private_directory(dir / "blobs");
      private_directory(dir / "parts");
      state.state = R::JobState::Uploading;
      state.core_life_id = core;
      write(dir, state);
    }
    const auto final = dir / "blobs" / c.digest;
    if (stdfs::exists(final)) {
      if (stdfs::file_size(final) != c.total)
        throw Error(R::ErrorCode::Rejected, "blob size conflict");
      durable_directory(dir / "blobs");
      durable_directory(dir / "parts");
      return;
    }
    uint64_t total = 0;
    size_t count = 0;
    for (const auto& folder : {dir / "blobs", dir / "parts"})
      for (const auto& file : stdfs::directory_iterator(folder)) {
        total += file.file_size();
        ++count;
      }
    if ((count >= 1024 && !stdfs::exists(dir / "parts" / c.digest)) ||
        c.bytes.size() > kMaxBlob - std::min(total, kMaxBlob))
      throw Error(R::ErrorCode::Rejected, "job upload budget exceeded");
    const auto part = dir / "parts" / c.digest;
    const auto offset = stdfs::exists(part) ? stdfs::file_size(part) : 0;
    const auto persist = [&](int fd, bool complete) {
      if (complete && file_digest(part) != c.digest) {
        stdfs::remove(part);
        throw Error(R::ErrorCode::Rejected, "blob SHA-256 mismatch");
      }
      // An acknowledged prefix is resumable at its recorded file length.
      if (::fsync(fd) != 0) throw std::runtime_error("cannot sync blob chunk");
      if (complete) {
        stdfs::rename(part, final);
        durable_directory(dir / "blobs");
      }
      if (complete || c.offset == 0) durable_directory(dir / "parts");
    };
    if (c.offset < offset) {
      const auto previous = fs::read_range(part, c.offset, c.bytes.size());
      if (!previous || *previous != std::string(reinterpret_cast<const char*>(
                                                    c.bytes.data()),
                                                c.bytes.size()))
        throw Error(R::ErrorCode::Rejected, "repeated blob chunk differs");
      Fd fd{::open(part.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW)};
      if (fd.value < 0) throw std::runtime_error("cannot reopen staged blob");
      persist(fd.value, offset == c.total);
      return;
    }
    if (c.offset != offset)
      throw Error(R::ErrorCode::Rejected, "blob chunks must be contiguous");
    Fd fd{::open(part.c_str(),
                 O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (fd.value < 0) throw std::runtime_error("cannot stage blob");
    size_t done = 0;
    while (done < c.bytes.size()) {
      const auto n =
          ::write(fd.value, c.bytes.data() + done, c.bytes.size() - done);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) throw std::runtime_error("cannot write blob chunk");
      done += static_cast<size_t>(n);
    }
    persist(fd.value, offset + c.bytes.size() == c.total);
  });
}
auto Journal::deliver_artifact(std::string_view job,
                               const R::JobArtifactRequestT& artifact,
                               std::string_view core) -> void {
  locked(job, [&](const auto& dir) {
    const auto state = read(dir);
    require_owner(state, core);
    if (state.fingerprint != artifact.fingerprint)
      throw Error(R::ErrorCode::Rejected, "artifact sent for a different job");
    const auto work = dir / "work";
    const auto wanted = fs::read_file(work / "artifact-wanted");
    // A cancelled or timed out call may have moved on by the time the
    // response arrives. A repeated response has no effect.
    if (state.state != R::JobState::Running || !wanted ||
        *wanted != artifact.id || stdfs::exists(work / "artifact-reply"))
      return;
    if (!crypto::is_uuid(artifact.id) ||
        (!artifact.found &&
         (!artifact.files.empty() || !artifact.directories.empty())) ||
        artifact.files.size() > 1024 || artifact.directories.size() > 2048)
      throw Error(R::ErrorCode::Invalid, "invalid remote artifact manifest");
    std::set<stdfs::path> paths;
    for (const auto& directory : artifact.directories)
      if (!paths.insert(remote::relative_path(directory)).second)
        throw Error(R::ErrorCode::Invalid, "duplicate remote artifact member");
    for (const auto& file : artifact.files) {
      if (!paths.insert(remote::relative_path(file->path)).second ||
          !crypto::is_sha256_hex(file->sha256))
        throw Error(R::ErrorCode::Invalid, "invalid remote artifact member");
      const auto blob = dir / "blobs" / file->sha256;
      if (!stdfs::is_regular_file(stdfs::symlink_status(blob)) ||
          stdfs::file_size(blob) != file->size)
        throw Error(R::ErrorCode::Rejected, "remote artifact blob is missing");
    }
    fs::write_atomic(work / "artifact-reply",
                     remote::encode<R::JobArtifactRequest>(artifact), 0600);
  });
}

auto Journal::get(std::string_view job, const R::GetBlobRequestT& r,
                  std::string_view core) const -> R::BlobChunkT {
  if (!r.limit || r.limit > kChunkBytes)
    throw Error(R::ErrorCode::Invalid, "invalid chunk limit");
  R::BlobChunkT result;
  locked(job, [&](const auto& dir) {
    require_owner(read(dir), core);
    const auto file = dir / "blobs" / r.digest;
    if (!stdfs::is_regular_file(stdfs::symlink_status(file)))
      throw Error(R::ErrorCode::NotFound, "job blob not found");
    result.digest = r.digest;
    result.total = stdfs::file_size(file);
    result.offset = r.offset;
    if (r.offset > result.total || result.total > kMaxBlob)
      throw Error(R::ErrorCode::Invalid, "invalid blob offset");
    const auto data = fs::read_range(
        file, r.offset, std::min<uint64_t>(r.limit, result.total - r.offset));
    if (!data) throw Error(R::ErrorCode::NotFound, "job blob not found");
    result.bytes.assign(data->begin(), data->end());
  });
  return result;
}
auto Journal::log(std::string_view job, uint64_t offset, uint32_t limit,
                  std::string_view core) const
    -> std::pair<uint64_t, std::string> {
  if (limit > kLogChunkBytes)
    throw Error(R::ErrorCode::Invalid, "invalid log chunk limit");
  std::pair<uint64_t, std::string> result;
  locked(job, [&](const auto& dir) {
    require_owner(read(dir), core);
    // The supervised invocation appends whole entries and flushes each one.
    Fd file{::open((dir / "work/result/logs").c_str(),
                   O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (file.value < 0) {
      if (errno == ENOENT || errno == ENOTDIR) return;
      throw Error(R::ErrorCode::Internal, "cannot open job log");
    }
    struct stat st{};
    if (::fstat(file.value, &st) != 0 || !S_ISREG(st.st_mode))
      throw Error(R::ErrorCode::Internal, "job log is not a regular file");
    result.first = static_cast<uint64_t>(st.st_size);
    // A cursor past the end returns no bytes; the size tells the caller.
    if (offset >= result.first) return;
    result.second.resize(static_cast<size_t>(
        std::min<uint64_t>(limit, result.first - offset)));
    size_t got = 0;
    while (got < result.second.size()) {
      const auto n = ::pread(file.value, result.second.data() + got,
                             result.second.size() - got,
                             static_cast<off_t>(offset + got));
      if (n < 0 && errno == EINTR) continue;
      if (n < 0) throw Error(R::ErrorCode::Internal, "cannot read job log");
      if (n == 0) break;
      got += static_cast<size_t>(n);
    }
    result.second.resize(got);
  });
  return result;
}
auto Journal::submit(std::string_view job, std::string_view bytes,
                     const R::JobSpecT& spec, std::string_view core) -> Record {
  const auto fingerprint = crypto::sha256_hex(bytes);
  Record result;
  locked(job, [&](const auto& dir) {
    result = read(dir);
    require_owner(result, core);
    if (result.state != R::JobState::Missing &&
        result.state != R::JobState::Uploading) {
      if (result.fingerprint != fingerprint)
        throw Error(R::ErrorCode::Rejected, "job fingerprint conflict");
      return;
    }
    for (const auto& blob : spec.input_blobs) {
      const auto file = dir / "blobs" / blob;
      if (!stdfs::is_regular_file(stdfs::symlink_status(file)))
        throw Error(R::ErrorCode::Rejected, "verified input blob is missing");
    }
    private_directory(dir);
    fs::write_atomic(dir / "spec.fb", bytes, 0600);
    result = {};
    result.state = R::JobState::Queued;
    result.core_life_id = core;
    result.fingerprint = fingerprint;
    write(dir, result);
  });
  return result;
}
auto Journal::cancel(std::string_view job, const std::string& fingerprint)
    -> Record {
  Record result;
  locked(job, [&](const auto& dir) {
    result = read(dir);
    if (result.state != R::JobState::Missing &&
        result.state != R::JobState::Uploading &&
        result.fingerprint != fingerprint)
      throw Error(R::ErrorCode::Rejected, "cancellation fingerprint conflict");
    if (terminal(result.state)) return;
    result.fingerprint = fingerprint;
    result.state = result.state == R::JobState::Running ||
                           result.state == R::JobState::Cancelling
                       ? R::JobState::Cancelling
                       : R::JobState::Cancelled;
    write(dir, result);
  });
  return result;
}
auto Journal::erase_scratch(const stdfs::path& dir) const -> void {
  for (const auto& entry : stdfs::directory_iterator(dir))
    if (entry.path().filename() != "state") discard_tree(entry.path());
  durable_directory(dir);
}
auto Journal::release(std::string_view job, const std::string& fingerprint)
    -> void {
  locked(job, [&](const auto& dir) {
    auto state = read(dir);
    if (state.state == R::JobState::Missing) return;
    if (!terminal(state.state) || state.fingerprint != fingerprint)
      throw Error(R::ErrorCode::Rejected,
                  "release requires a terminal job and matching fingerprint");
    state = Record{};
    state.state = R::JobState::Released;
    state.fingerprint = fingerprint;
    write(dir, state);  // commit tombstone before erasing; recovery finishes an
                        // interrupted erase
    erase_scratch(dir);
  });
}
auto Journal::claim(std::string_view job, uint64_t pid, uint64_t start,
                    const std::string& boot) -> bool {
  bool claimed = false;
  locked(job, [&](const auto& dir) {
    auto state = read(dir);
    if (state.state != R::JobState::Queued) return;
    state.state = R::JobState::Running;
    state.pid = pid;
    state.start = start;
    state.boot = boot;
    write(dir, state);
    claimed = true;
  });
  return claimed;
}
auto Journal::supervisor_identity(std::string_view job, uint64_t pid,
                                  uint64_t start, const std::string& boot)
    -> void {
  locked(job, [&](const auto& dir) {
    auto state = read(dir);
    if (state.state != R::JobState::Running &&
        state.state != R::JobState::Cancelling)
      throw std::runtime_error("supervisor no longer owns an active job");
    state.pid = pid;
    state.start = start;
    state.boot = boot;
    write(dir, state);
  });
}
auto Journal::finish(std::string_view job, R::JobState outcome,
                     const std::string& result, const std::string& diagnostic)
    -> void {
  if (outcome != R::JobState::Succeeded && outcome != R::JobState::Failed &&
      outcome != R::JobState::Interrupted)
    throw std::runtime_error("invalid terminal outcome");
  locked(job, [&](const auto& dir) {
    auto state = read(dir);
    if (terminal(state.state)) return;
    if (state.state != R::JobState::Running &&
        state.state != R::JobState::Cancelling &&
        state.state != R::JobState::Queued)
      throw std::runtime_error("job was not accepted");
    state.state = state.state == R::JobState::Cancelling
                      ? R::JobState::Cancelled
                      : outcome;
    state.result = state.state == R::JobState::Succeeded ? result : "";
    state.diagnostic = state.state == R::JobState::Cancelled ? "" : diagnostic;
    write(dir, state);
  });
}
auto Journal::clean_released(std::string_view job) -> void {
  locked(job, [&](const auto& dir) {
    auto state = read(dir);
    if (state.state == R::JobState::Released) erase_scratch(dir);
  });
}
auto Journal::discard(std::string_view job, bool active) -> Record {
  Record previous;
  locked(job, [&](const auto& dir) {
    if (!stdfs::exists(stdfs::symlink_status(dir)))
      throw Error(R::ErrorCode::NotFound, "job not found");
    // An unreadable record has no state to protect; its fingerprint is lost.
    try {
      previous = read(dir);
    } catch (const std::runtime_error&) {
    }
    if (!active && (previous.state == R::JobState::Queued ||
                    previous.state == R::JobState::Running ||
                    previous.state == R::JobState::Cancelling))
      throw Error(R::ErrorCode::Rejected,
                  "the journal records this job as queued, running or "
                  "cancelling");
    Record tombstone;
    tombstone.state = R::JobState::Released;
    tombstone.fingerprint = previous.fingerprint;
    write(dir, tombstone);
    erase_scratch(dir);
  });
  return previous;
}
auto Journal::import_results(std::string_view job) -> std::string {
  const auto dir = job_path(job);
  const auto results = dir / "work/result";
  const auto listing = fs::list_beneath(results, {}, 65536 + 64);
  if (!listing.links.empty() || !listing.special.empty() ||
      listing.files.size() > 65536)
    throw std::runtime_error("unsupported or excessive result entries");
  R::ResultManifestT manifest;
  // Staging applies the Card output budget. Leave room for the independently
  // bounded worker reply, environment metadata and captured log.
  constexpr uint64_t kMaxResult = kMaxBlob + (64ull << 20);
  uint64_t total = 0;
  for (const auto& name : listing.files) {
    const stdfs::path relative(name);
    if (!fs::valid_relative_path(name) || name.size() > 4096 ||
        std::distance(relative.begin(), relative.end()) > 64)
      throw std::runtime_error("invalid result path");
    for (const auto& part : relative)
      if (part.string().size() > 255)
        throw std::runtime_error("result component too long");
    const auto file = results / relative;
    const auto size = stdfs::file_size(file);
    if (size > kMaxBlob || size > kMaxResult - total)
      throw std::runtime_error("remote result exceeds the transfer limit");
    total += size;
    const auto digest = file_digest(file);
    if (name == "worker.fb") {
      manifest.invocation_result_sha256 = digest;
    } else {
      auto entry = std::make_unique<R::ResultFileT>();
      entry->path = name;
      entry->sha256 = digest;
      entry->size = size;
      manifest.outputs.push_back(std::move(entry));
    }
    // The namespace has exited and the worker is gone. Run data never enter
    // Nix.
    const auto destination = dir / "blobs" / digest;
    if (!stdfs::exists(destination)) stdfs::copy_file(file, destination);
    Fd persisted{
        ::open(destination.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (persisted.value < 0 || ::fsync(persisted.value) != 0)
      throw std::runtime_error("cannot sync result blob");
  }
  if (manifest.invocation_result_sha256.empty())
    throw std::runtime_error("worker result is missing");
  flatbuffers::FlatBufferBuilder b;
  b.Finish(R::ResultManifest::Pack(b, &manifest));
  if (b.GetSize() > (8 << 20))
    throw std::runtime_error("result manifest exceeds 8 MiB");
  std::string bytes(reinterpret_cast<const char*>(b.GetBufferPointer()),
                    b.GetSize());
  const auto digest = crypto::sha256_hex(bytes);
  fs::write_atomic(dir / "blobs" / digest, bytes, 0600);
  durable_directory(dir / "blobs");
  return digest;
}
}  // namespace tsugumi::workerd
