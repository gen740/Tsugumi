// Collect Card values: an ordered list of input values gathered into one
// output value, without opening or rebuilding any member.
//
// FlatBuffer inputs become `tsugumi.builtin.Collected { items:[CardPort] }`,
// one opaque envelope per element (the Card ABI's CardPort: its ordinal as
// name, kind FlatBuffer and the element's bytes).
//
// Path inputs are copied into a new Path output, one directory with an
// ordinal folder per element (`0/<basename>`, `1/<basename>`, ...), each a
// regular file or a whole directory as found.
#pragma once

#include <atomic>
#include <filesystem>
#include <span>
#include <string>

#include "model.hh"
#include "tsugumi/document/collect_type.hh"
#include "tsugumi/document/types.hh"

namespace tsugumi {

struct CollectFileLimits {
  size_t files = 1000;
  size_t entries = 10000;  // Files, directories and members together.
};

// Collects `members` (values of the `source` port, in order) into one value of
// the `output` port. Path members must live below
// one of `file_roots`; their copies are staged beside
// `output_dir` (which must not exist yet) and moved there only once complete.
// Throws Failed (cancelled, output_invalid or internal_error).
auto collect_values(const types::PortType& source,
                    const types::PortType& output,
                    std::span<const Value> members,
                    std::span<const std::filesystem::path> file_roots,
                    const std::filesystem::path& output_dir,
                    const std::atomic<bool>& cancel,
                    CollectFileLimits limits = {}) -> Value;

}  // namespace tsugumi
