#pragma once

#include <filesystem>

#include "tsugumi/util/sqlite.hh"

namespace tsugumi::host {
// One connection for host state and Home conversations. Callers serialize all
// access with the shared StateGate, including cross-component transactions.
struct HostStore {
  explicit HostStore(const std::filesystem::path& directory);
  sql::Db db;
};
}  // namespace tsugumi::host
