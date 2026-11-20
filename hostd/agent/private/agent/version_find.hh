#pragma once

#include <functional>
#include <string>

#include "tsugumi/util/json_object.hh"

namespace tsugumi::agent {

// One project read: the reply's value, or host::Failure for a failure reply.
using ProjectRead = std::function<json::Members(const std::string& name,
                                                const json::Members& args)>;

// version_find narrows recorded history to candidate trials by composing
// existing reads: version_graph (which versions changed `card`, and their
// runs), version_note (how it changed) and run_record (what `observe`
// recorded in those runs and in the nearest earlier version's run). It
// compares outputs by recorded content hash only and says what it could not
// read. It stores nothing and summarizes nothing. Returns the value JSON.
auto FindVersions(const json::Members& args, const ProjectRead& read)
    -> std::string;

}  // namespace tsugumi::agent
