#include "journal.hh"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>

#include "tsugumi/util/json.hh"
#include "tsugumi/protocol/worker_generated.h"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi::workerd {
namespace {
struct StateJson {
  schema::t::Uint64<"state"> state;
  schema::t::String<"fingerprint"> fingerprint;
  schema::t::String<"result"> result;
  schema::t::String<"diagnostic"> diagnostic;
  schema::t::Uint64<"deadline"> deadline;
  schema::t::Uint64<"pid"> pid;
  schema::t::Uint64<"start"> start;
  schema::t::String<"boot"> boot;
  schema::t::String<"core_life_id"> core_life_id;
  schema::t::Uint64<"heartbeat_interval_seconds"> heartbeat_interval_seconds;
  schema::t::Uint64<"missed_heartbeats"> missed_heartbeats;
};
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
auto check_id(std::string_view value) -> void {
  if (!crypto::is_sha256_hex(value))
    throw Error(R::ErrorCode::Invalid, "expected a 64-hex identity");
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
  check_id(value);
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
  private_directory(root_ / "cores");
  instance_ = identity(root_, "instance_id");
}
auto Journal::job_path(std::string_view job) const -> stdfs::path {
  check_id(job);
  return root_ / "jobs" / job;
}
auto Journal::locked(
    std::string_view job,
    const std::function<void(const stdfs::path&)>& action) const -> void {
  check_id(job);
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
  StateJson s;
  if (schema::decode(bounded_file(directory / "state", 16384), s) ||
      *s.state > static_cast<uint64_t>(R::JobState::Released))
    throw std::runtime_error("corrupt job journal");
  return {static_cast<R::JobState>(*s.state),
          *s.fingerprint,
          *s.result,
          *s.diagnostic,
          *s.deadline,
          *s.pid,
          *s.start,
          *s.boot,
          *s.core_life_id,
          static_cast<uint32_t>(*s.heartbeat_interval_seconds),
          static_cast<uint32_t>(*s.missed_heartbeats)};
}
auto Journal::write(const stdfs::path& directory, const Record& r) const
    -> void {
  private_directory(directory);
  durable_directory(directory.parent_path());
  StateJson s;
  *s.state = static_cast<uint64_t>(r.state);
  *s.fingerprint = r.fingerprint;
  *s.result = r.result;
  *s.diagnostic = r.diagnostic.substr(0, 4096);
  *s.deadline = r.deadline;
  *s.pid = r.pid;
  *s.start = r.start;
  *s.boot = r.boot;
  *s.core_life_id = r.core_life_id;
  *s.heartbeat_interval_seconds = r.heartbeat_interval_seconds;
  *s.missed_heartbeats = r.missed_heartbeats;
  fs::write_atomic(directory / "state", schema::encode(s), 0600);
}
auto Journal::status(std::string_view job) const -> Record {
  Record result;
  locked(job, [&](const auto& directory) { result = read(directory); });
  return result;
}
auto Journal::spec(std::string_view job) const -> std::string {
  return bounded_file(job_path(job) / "spec.fb", kMaxSpec);
}
auto Journal::touch_core(std::string_view core, uint64_t now) -> void {
  check_id(core);
  // Serialize renewals across carriers. Cancel the expired life's jobs before
  // renewal, even if its reconnect arrives before the next listener tick.
  Fd lock{::open((root_ / "cores" / (std::string(core) + ".lock")).c_str(),
                 O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)};
  if (lock.value < 0 || ::flock(lock.value, LOCK_EX) != 0)
    throw std::runtime_error("cannot lock core lease");
  const auto path = root_ / "cores" / core;
  for (const auto& job : jobs())
    if (status(job).core_life_id == core) expire_core(job, now);
  fs::write_atomic(path, std::to_string(now), 0600);
}

auto Journal::require_owner(std::string_view job, std::string_view core) const
    -> void {
  check_id(core);
  const auto state = status(job);
  if (!state.core_life_id.empty() && state.core_life_id != core)
    throw Error(R::ErrorCode::Rejected, "job belongs to another core life");
}

auto Journal::expire_core(std::string_view job, uint64_t now) -> void {
  locked(job, [&](const auto& dir) {
    auto state = read(dir);
    if (state.core_life_id.empty() || terminal(state.state) ||
        state.state == R::JobState::Uploading || !state.missed_heartbeats)
      return;
    check_id(state.core_life_id);
    const auto lease = fs::read_file(root_ / "cores" / state.core_life_id);
    if (lease) {
      const auto last = std::stoull(*lease);
      constexpr auto grace = static_cast<uint64_t>(kHeartbeatGrace.count());
      // Divide instead of multiplying two uint32 settings and milliseconds.
      if (now < last || now - last < grace ||
          (now - last - grace) / 1000 / state.heartbeat_interval_seconds <
                            state.missed_heartbeats) return;
    }
    // Use precisely the ordinary cancellation transition and data policy.
    state.state = state.state == R::JobState::Running ||
                          state.state == R::JobState::Cancelling
                      ? R::JobState::Cancelling
                      : R::JobState::Cancelled;
    write(dir, state);
  });
}

auto Journal::put(std::string_view job, const R::PutBlobRequestT& c,
                  std::string_view core) -> void {
  check_id(core);
  check_id(c.digest);
  if (c.total > kMaxBlob || c.bytes.size() > kChunkBytes ||
      c.offset > c.total || c.bytes.size() > c.total - c.offset ||
      (c.bytes.empty() && c.total != 0))
    throw Error(R::ErrorCode::Invalid, "invalid blob chunk bounds");
  locked(job, [&](const auto& dir) {
    auto state = read(dir);
    if (!state.core_life_id.empty() && state.core_life_id != core)
      throw Error(R::ErrorCode::Rejected, "job belongs to another core life");
    state.core_life_id = core;
    if (state.state != R::JobState::Missing &&
        state.state != R::JobState::Uploading)
      throw Error(R::ErrorCode::Rejected, "job no longer accepts uploads");
    if (state.state == R::JobState::Missing) {
      state.state = R::JobState::Uploading;
    }
    state.deadline = now_ms() + 3600000;
    write(dir, state);
    private_directory(dir / "blobs");
    private_directory(dir / "parts");
    const auto final = dir / "blobs" / c.digest;
    if (stdfs::exists(final)) {
      if (stdfs::file_size(final) != c.total)
        throw Error(R::ErrorCode::Rejected, "blob size conflict");
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
    if (c.offset < offset) {
      const auto previous = fs::read_range(part, c.offset, c.bytes.size());
      if (!previous || *previous != std::string(reinterpret_cast<const char*>(
                                                    c.bytes.data()),
                                                c.bytes.size()))
        throw Error(R::ErrorCode::Rejected, "repeated blob chunk differs");
      if (offset == c.total) {
        if (file_digest(part) != c.digest)
          throw Error(R::ErrorCode::Rejected, "blob SHA-256 mismatch");
        stdfs::rename(part, final);
        durable_directory(dir / "blobs");
        durable_directory(dir / "parts");
      }
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
    if (::fsync(fd.value) != 0)
      throw std::runtime_error("cannot sync blob chunk");
    if (offset + c.bytes.size() == c.total) {
      if (file_digest(part) != c.digest) {
        stdfs::remove(part);
        throw Error(R::ErrorCode::Rejected, "blob SHA-256 mismatch");
      }
      stdfs::rename(part, final);
      durable_directory(dir / "blobs");
      durable_directory(dir / "parts");
    }
  });
}
auto Journal::get(std::string_view job, const R::GetBlobRequestT& r) const
    -> R::BlobChunkT {
  check_id(r.digest);
  if (!r.limit || r.limit > kChunkBytes)
    throw Error(R::ErrorCode::Invalid, "invalid chunk limit");
  R::BlobChunkT result;
  locked(job, [&](const auto& dir) {
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
auto Journal::submit(std::string_view job, std::string_view bytes,
                     std::string_view core) -> Record {
  check_id(core);
  const auto fingerprint = crypto::sha256_hex(bytes);
  Record result;
  locked(job, [&](const auto& dir) {
    result = read(dir);
    if (!result.core_life_id.empty() && result.core_life_id != core)
      throw Error(R::ErrorCode::Rejected, "job belongs to another core life");
    if (result.state != R::JobState::Missing &&
        result.state != R::JobState::Uploading) {
      if (result.fingerprint != fingerprint)
        throw Error(R::ErrorCode::Rejected, "job fingerprint conflict");
      return;
    }
    const auto s = parse_spec(bytes);
    for (const auto& blob : s->input_blobs) {
      const auto file = dir / "blobs" / blob;
      if (!stdfs::is_regular_file(stdfs::symlink_status(file)))
        throw Error(R::ErrorCode::Rejected, "verified input blob is missing");
    }
    private_directory(dir);
    fs::write_atomic(dir / "spec.fb", bytes, 0600);
    result = {};
    result.state = R::JobState::Queued;
    result.core_life_id = core;
    result.heartbeat_interval_seconds = s->heartbeat_interval_seconds;
    result.missed_heartbeats = s->missed_heartbeats;
    result.fingerprint = fingerprint;
    result.deadline = 0;
    write(dir, result);
  });
  return result;
}
auto Journal::cancel(std::string_view job, const std::string& fingerprint)
    -> Record {
  check_id(fingerprint);
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
  check_id(fingerprint);
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
    if (state.state != R::JobState::Queued)
      return;
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
auto Journal::expire(std::string_view job) -> void {
  locked(job, [&](const auto& dir) {
    auto state = read(dir);
    if (state.state == R::JobState::Released) {
      erase_scratch(dir);
      return;
    }
    if (!state.deadline || state.deadline > now_ms()) return;
    if (state.state == R::JobState::Uploading) {
      // Never accepted: no job fingerprint or result to recover.
      discard_tree(dir);
    }
  });
}
auto Journal::import_results(std::string_view job) -> std::string {
  const auto dir = job_path(job);
  const auto results = dir / "work/result";
  const auto listing = fs::list_beneath(results, {}, 65536 + 64);
  if (!listing.links.empty() || !listing.special.empty() ||
      listing.files.size() > 65536)
    throw std::runtime_error("unsupported or excessive result entries");
  R::ResultManifestT manifest;
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
    if (size > kMaxBlob - total)
      throw std::runtime_error("result exceeds 64 GiB");
    total += size;
    const auto digest = file_digest(file);
    if (name == "worker.fb") {
      auto bytes = bounded_file(file, 16 << 20);
      flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(bytes.data()),
                              bytes.size());
      if (!Tsugumi::Worker::VerifyWorkerMessageBuffer(v))
        throw std::runtime_error("invalid worker result");
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
