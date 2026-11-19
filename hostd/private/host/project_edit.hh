#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "host/save.hh"
#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {
// Replaces only the selected JSON span. All untouched bytes, including numeric
// spellings, whitespace, pins and hashes, survive unchanged.
auto EditProjectField(const std::string& document,
                      const Tsugumi::Host::ProjectFieldEditT& edit)
    -> std::string;
// Carry a transformation's changed values into the original document spans.
auto PreserveProjectBytes(const std::string& before, const std::string& after)
    -> std::string;
using EditFileRead =
    std::function<std::optional<std::string>(const std::string&)>;
// Guarded reverse edits for the exact concrete plan accepted by the host.
// JSON fields are addressed by identity so unrelated later edits survive Undo.
auto ReverseProjectChanges(const ::tsugumi::json::Members& plan,
                           const SaveBase& base, const EditFileRead& read)
    -> Tsugumi::Host::ProjectEditsT;
using EditFiles = std::map<std::string, std::optional<std::string>>;
auto ProjectPaths(const SaveBase& base) -> std::set<std::string>;
// Convert confirmed before and after bytes into the core's save changes.
auto ProjectSaveChanges(const SaveBase& base, const EditFiles& originals,
                        const EditFiles& files,
                        const std::set<std::string>& removed_definitions)
    -> ::tsugumi::json::Members;
// Prepare one atomic concrete save against a revision-pinned confirmed state.
auto PrepareProjectEdits(
    const Tsugumi::Host::ProjectEditsT& edits, const SaveBase& base,
    const EditFileRead& read,
    const std::function<void(const std::string&)>& authorize = {})
    -> ::tsugumi::json::Members;
}  // namespace tsugumi::host
