#pragma once

#include <string>

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {
// Returns a new definition and its text files; never writes the project.
auto WriteTemplate(const std::string &name, const std::string &id)
    -> Tsugumi::Host::DocumentTemplateT;
} // namespace tsugumi::host
