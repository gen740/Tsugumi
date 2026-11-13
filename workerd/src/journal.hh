#pragma once

#include <filesystem>
#include <functional>
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
  uint64_t deadline = 0;
  uint64_t pid = 0;
  uint64_t start = 0;
  std::string boot;
  auto status() const -> R::JobStatusT;
};

auto now_ms() -> uint64_t;
auto private_directory(const stdfs::path& path) -> void;
auto identity(const stdfs::path& directory, const std::string& name)
    -> std::string;
auto check_id(std::string_view value) -> void;
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
  auto jobs() const -> std::vector<std::string>;
  auto status(std::string_view job) const -> Record;
  auto spec(std::string_view job) const -> std::string;
  auto put(std::string_view job, const R::PutBlobRequestT& chunk) -> void;
  auto get(std::string_view job, const R::GetBlobRequestT& request) const
      -> R::BlobChunkT;
  auto submit(std::string_view job, std::string_view spec) -> Record;
  auto cancel(std::string_view job, const std::string& fingerprint) -> Record;
  auto release(std::string_view job, const std::string& fingerprint) -> void;
  auto claim(std::string_view job, uint64_t pid, uint64_t start,
             const std::string& boot) -> bool;
  auto supervisor_identity(std::string_view job, uint64_t pid, uint64_t start,
                           const std::string& boot) -> void;
  auto finish(std::string_view job, R::JobState state,
              const std::string& result, const std::string& diagnostic) -> void;
  auto expire(std::string_view job) -> void;
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
