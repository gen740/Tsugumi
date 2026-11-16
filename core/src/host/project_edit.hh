#pragma once

#include <functional>
#include <optional>
#include <string>

#include "host/save.hh"
#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {
// Replaces only the selected JSON span. All untouched bytes, including numeric
// spellings, whitespace, pins and hashes, survive unchanged.
auto EditProjectField(const std::string& document,
                      const Tsugumi::Host::ProjectFieldEditT& edit)
    -> std::string;
using EditFileRead =
    std::function<std::optional<std::string>(const std::string&)>;
// Prepare one atomic concrete save against a revision-pinned confirmed state.
auto PrepareProjectEdits(
    const Tsugumi::Host::ProjectEditsT& edits, const SaveBase& base,
    const EditFileRead& read,
    const std::function<void(const std::string&)>& authorize = {})
    -> agent::json::Members;
}  // namespace tsugumi::host
