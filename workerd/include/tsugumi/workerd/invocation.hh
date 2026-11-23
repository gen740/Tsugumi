// The remote job files (protocol/remote_worker.fbs): one FlatBuffer each, and
// the result's log as a sequence of size-prefixed LogEntry buffers.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "tsugumi/protocol/remote_worker_generated.h"
#include "tsugumi/protocol/tsugumi_enums_generated.h"
#include "tsugumi/util/flat_file.hh"

namespace tsugumi {
namespace remote {

namespace RW = Tsugumi::RemoteWorker;

auto relative_path(std::string_view path) -> std::filesystem::path;

// A job file's bytes, and its verified value.
using flat::decode;
using flat::encode;

// One log entry: its uint32 little-endian size, then its LogEntry buffer.
auto encode_log_entry(Tsugumi::Data::LogStream stream, std::string_view text)
    -> std::string;
// Calls `entry` for each complete entry at the start of `bytes`, with the
// entry's bytes including its size, and returns the bytes those entries take.
// Throws std::runtime_error for an entry that is not a LogEntry.
auto decode_log_entries(
    std::string_view bytes,
    const std::function<void(Tsugumi::Data::LogStream, std::string,
                             std::string_view)>& entry) -> size_t;

}  // namespace remote
}  // namespace tsugumi
