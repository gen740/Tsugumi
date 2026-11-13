// Collect Card values: an ordered list of input values gathered into one
// output value.
//
// FlatBuffer inputs become `tsugumi.collect.Collected { items:[In] (required) }`
// whose items point to copies of the verified input tables.
//
// File / Directory inputs are copied into a new Directory output, one ordinal
// folder per element (`0/<basename>`, `1/<basename>`, ...), described by
// `tsugumi.collect.Directory { value:string; items:[Entry] }` where each
// Entry holds the element's original basename and its path relative to
// `value`.
#pragma once

#include <atomic>
#include <filesystem>
#include <span>
#include <string>

#include "model.hh"
#include "types.hh"

namespace tsugumi {

// The output message type of a Collect Card whose input port is `source`.
// Returns null with `error` set when the source type cannot be collected.
auto collect_output_type(const types::PortType& source, std::string& error) -> types::TypeRef;

// Collects `members` (values of the `source` port, in order) into one value of
// the `output` port. File / Directory members must live below
// one of `file_roots`; their copies are staged beside
// `output_dir` (which must not exist yet) and moved there only once complete.
// Throws Failed (cancelled, output_invalid or internal_error).
auto collect_values(const types::PortType& source, const types::PortType& output,
                    std::span<const Value> members, std::span<const std::filesystem::path> file_roots,
                    const std::filesystem::path& output_dir, const std::atomic<bool>& cancel) -> Value;

}  // namespace tsugumi
