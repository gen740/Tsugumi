#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

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
auto file_digest(const stdfs::path& path, uint64_t limit = kMaxBlob)
    -> std::string;

// All mutations use a per-job flock. A separate supervisor lock is held for
// the supervisor's lifetime, including while the listening daemon is absent.
class Journal {
 public:
  explicit Journal(stdfs::path root);
  auto root() const -> const stdfs::path& { return root_; }
  auto instance() const -> const std::string& { return instance_; }
  auto job_path(std::string_view job) const -> stdfs::path;
  // Held by a job's execution supervisor for its whole lifetime.
  auto supervisor_lock_path(std::string_view job) const -> stdfs::path;
  auto jobs() const -> std::vector<std::string>;
  auto status(std::string_view job,
              std::optional<std::string_view> core = std::nullopt) const
      -> Record;
  auto spec(std::string_view job) const -> std::string;
  auto put(std::string_view job, const R::PutBlobRequestT& chunk,
           std::string_view core) -> void;
  auto get(std::string_view job, const R::GetBlobRequestT& request,
           std::string_view core) const -> R::BlobChunkT;
  auto submit(std::string_view job, std::string_view bytes,
              const R::JobSpecT& spec, std::string_view core) -> Record;
  auto cancel(std::string_view job, const std::string& fingerprint) -> Record;
  auto release(std::string_view job, const std::string& fingerprint) -> void;
  auto claim(std::string_view job, uint64_t pid, uint64_t start,
             const std::string& boot) -> bool;
  auto supervisor_identity(std::string_view job, uint64_t pid, uint64_t start,
                           const std::string& boot) -> void;
  auto finish(std::string_view job, R::JobState state,
              const std::string& result, const std::string& diagnostic) -> void;
  auto clean_released(std::string_view job) -> void;
  // A person's removal on the worker machine. Like release, it commits a
  // tombstone that keeps rejecting a delayed submission, then erases the
  // scratch. A queued, running or cancelling record is kept unless `active`.
  // The caller holds the supervisor lock, so no supervisor runs the job.
  // Returns the record that was replaced.
  auto discard(std::string_view job, bool active) -> Record;
  // Called only after the sandbox has stopped. Returns a verified manifest.
  auto import_results(std::string_view job) -> std::string;

 private:
  auto locked(std::string_view job,
              const std::function<void(const stdfs::path&)>& action) const
      -> void;
  auto read(const stdfs::path& directory) const -> Record;
  auto write(const stdfs::path& directory, const Record& record) const -> void;
  auto erase_scratch(const stdfs::path& directory) const -> void;
  stdfs::path root_;
  std::string instance_;
};
}  // namespace tsugumi::workerd
