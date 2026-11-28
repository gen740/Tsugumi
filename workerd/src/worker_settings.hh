// The worker's own settings: STATE/settings.json, FlatBuffers JSON text with
// the schema protocol/worker_settings.fbs. The listener reads the file at
// start; its dashboard replaces it. Concurrency and pull caches apply to the
// next job that starts; the rest when the listener starts again.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include "tsugumi/protocol/worker_settings_generated.h"

namespace tsugumi::workerd {

namespace WS = Tsugumi::WorkerSettings;

class WorkerSettings {
 public:
  // Reads STATE/settings.json, creating it with the defaults when it is
  // missing. Throws std::runtime_error naming the file and the problem.
  explicit WorkerSettings(const std::filesystem::path& root);

  // The file's current values.
  auto saved() const -> WS::SettingsT;
  // The values this listener started with.
  auto started() const -> const WS::SettingsT& { return started_; }
  // Checks the values, then replaces the file atomically (mode 0600).
  auto save(WS::SettingsT settings) -> void;
  // The concurrency in effect, which follows saves.
  auto max_jobs() const -> uint32_t { return max_jobs_.load(); }
  // Whether a saved value that takes effect at start differs from this
  // listener's.
  auto restart_required() const -> bool;
  auto file() const -> const std::filesystem::path& { return file_; }

  static auto defaults() -> WS::SettingsT;
  // Throws std::invalid_argument naming the first value it refuses.
  static auto validate(const WS::SettingsT& settings) -> void;
  // Throws std::runtime_error with the line and column of a schema error.
  static auto parse(std::string_view text) -> WS::SettingsT;
  static auto print(const WS::SettingsT& settings) -> std::string;

 private:
  std::filesystem::path file_;
  mutable std::mutex mutex_;
  WS::SettingsT saved_, started_;
  std::atomic<uint32_t> max_jobs_;
};

}  // namespace tsugumi::workerd
