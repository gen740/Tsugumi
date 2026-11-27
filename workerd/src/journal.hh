#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "tsugumi/util/sha256_hex.hh"
#include "tsugumi/workerd/protocol.hh"

namespace tsugumi::workerd {
namespace stdfs = std::filesystem;

struct Record {
  R::JobState state = R::JobState::Missing;
  std::string fingerprint;
  std::string result;
  std::string diagnostic;
  uint64_t pid = 0;
  uint64_t start = 0;
  std::string boot;
  std::string core_life_id;
  auto status() const -> R::JobStatusT;
};

auto now_ms() -> uint64_t;
auto private_directory(const stdfs::path& path) -> void;
auto identity(const stdfs::path& directory, const std::string& name)
    -> std::string;
auto discard_tree(const stdfs::path& path) -> void;

// All mutations use a per-job flock. A separate supervisor lock is held for
// the supervisor's lifetime, including while the listening daemon is absent.
class Journal {
 public:
  explicit Journal(stdfs::path root);
  auto root() const -> const stdfs::path& { return root_; }
  auto instance() const -> const std::string& { return instance_; }
  auto job_path(const Sha256Hex& job) const -> stdfs::path;
  // Held by a job's execution supervisor for its whole lifetime.
  auto supervisor_lock_path(const Sha256Hex& job) const -> stdfs::path;
  auto jobs() const -> std::vector<Sha256Hex>;
  auto status(const Sha256Hex& job,
              std::optional<std::string_view> core = std::nullopt) const
      -> Record;
  auto spec(const Sha256Hex& job) const -> std::string;
  auto put(const Sha256Hex& job, const Sha256Hex& digest, uint64_t total,
           uint64_t offset, std::span<const uint8_t> bytes,
           std::string_view core) -> void;
  auto deliver_artifact(const Sha256Hex& job,
                        const R::JobArtifactRequestT& artifact,
                        std::string_view core) -> void;
  auto get(const Sha256Hex& job, const Sha256Hex& digest, uint64_t offset,
           uint32_t limit, std::string_view core) const -> R::BlobChunkT;
  // Bytes of the running or kept job log from `offset`, at most `limit`, and
  // the log's current size. A job without a log yet has size zero.
  auto log(const Sha256Hex& job, uint64_t offset, uint32_t limit,
           std::string_view core) const -> std::pair<uint64_t, std::string>;
  auto submit(const Sha256Hex& job, std::string_view bytes,
              std::span<const Sha256Hex> input_blobs, std::string_view core)
      -> Record;
  auto cancel(const Sha256Hex& job, const std::string& fingerprint) -> Record;
  auto release(const Sha256Hex& job, const std::string& fingerprint) -> void;
  auto claim(const Sha256Hex& job, uint64_t pid, uint64_t start,
             const std::string& boot) -> bool;
  auto supervisor_identity(const Sha256Hex& job, uint64_t pid, uint64_t start,
                           const std::string& boot) -> void;
  auto finish(const Sha256Hex& job, R::JobState state,
              const std::string& result, const std::string& diagnostic) -> void;
  auto clean_released(const Sha256Hex& job) -> void;
  // A person's removal on the worker machine. Like release, it commits a
  // tombstone that keeps rejecting a delayed submission, then erases the
  // scratch. A queued, running or cancelling record is kept unless `active`.
  // The caller holds the supervisor lock, so no supervisor runs the job.
  // Returns the record that was replaced.
  auto discard(const Sha256Hex& job, bool active) -> Record;
  // Called only after the sandbox has stopped. Returns a verified manifest.
  auto import_results(const Sha256Hex& job) -> std::string;

 private:
  enum class Access { Quiet, NotifyScheduler };
  auto locked(const Sha256Hex& job, Access access,
              const std::function<void(const stdfs::path&)>& action) const
      -> void;
  auto read(const stdfs::path& directory) const -> Record;
  auto write(const stdfs::path& directory, const Record& record) const -> void;
  auto erase_scratch(const stdfs::path& directory) const -> void;
  struct UploadUsage {
    uint64_t bytes = 0;
    size_t files = 0;
  };
  std::mutex upload_usage_mutex_;
  std::map<stdfs::path, UploadUsage> upload_usage_;
  stdfs::path root_;
  std::string instance_;
};
}  // namespace tsugumi::workerd
