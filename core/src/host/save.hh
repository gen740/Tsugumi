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
struct SaveInspection {
  agent::json::Members snapshot;
  SaveBase base;
};
// An inspection is usable only after its external documents have been read at
// the same revision. Discard a partial capture when another writer intervenes.
auto InspectForSave(const SaveRead& read) -> SaveInspection;
}  // namespace tsugumi::host
