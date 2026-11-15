#pragma once

#include <functional>
#include <string>

#include "agent/json.hh"

namespace tsugumi::host {
auto CardStem(const std::string& name) -> std::string;
// Only definition documents and source hashes; never execution records.
struct SaveBase {
  agent::json::Members snapshot;
  agent::json::Members files;  // path -> JSON document value
};
using SaveRead = std::function<agent::json::Members(
    const std::string&, const agent::json::Members&)>;
auto CaptureSaveBase(const agent::json::Members& snapshot, const SaveRead& read)
    -> SaveBase;
// Apply the acknowledged delta and the submitted document bytes without reading
// a later revision that another client may already have saved.
auto AdvanceSaveBase(const SaveBase& base, const agent::json::Members& result,
                     const agent::json::Members& arguments) -> SaveBase;
auto RebaseSave(agent::json::Members args, const SaveBase& base,
                const SaveBase& latest) -> agent::json::Members;
}  // namespace tsugumi::host
