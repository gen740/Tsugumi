#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "tsugumi/util/fs.hh"

namespace tsugumi::host {

struct LockHolder {
  int pid;
  std::string application;

  auto Description() const -> std::string;
};

// flock is the authority. A separately locked record authenticates the PID
// without treating a record left behind by an exited process as current.
class ProcessLock {
 public:
  ProcessLock(ProcessLock&& other) noexcept;
  ~ProcessLock();

  static auto TryAcquire(const std::filesystem::path& path,
                         std::string_view application,
                         std::optional<LockHolder>& holder)
      -> std::optional<ProcessLock>;

 private:
  explicit ProcessLock(fs::FileLock lock);

  fs::FileLock lock_;
  int record_ = -1;
};

}  // namespace tsugumi::host
