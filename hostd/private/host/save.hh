#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "tsugumi/util/json_object.hh"

namespace tsugumi::host {
// One save may inspect the same patch in policy, exclusions and layout.
class SourcePatches {
 public:
  auto Project(std::string_view source,
               const std::vector<std::string>& patches) const
      -> const std::string&;

 private:
  mutable std::map<std::pair<std::string, std::vector<std::string>>,
                   std::string>
      projected_;
};

auto CardStem(const std::string& name) -> std::string;
// Only definition documents and source hashes; never execution records.
struct SaveBase {
  ::tsugumi::json::Members snapshot;
  ::tsugumi::json::Members files;  // path -> JSON document value
};
using SaveRead = std::function<::tsugumi::json::Members(
    const std::string&, const ::tsugumi::json::Members&)>;
auto CaptureSaveBase(const ::tsugumi::json::Members& snapshot,
                     const SaveRead& read) -> SaveBase;
struct SaveInspection {
  ::tsugumi::json::Members snapshot;
  SaveBase base;
};
// An inspection is usable only after its external documents have been read at
// the same revision. Discard a partial capture when another writer intervenes.
auto InspectForSave(const SaveRead& read) -> SaveInspection;
}  // namespace tsugumi::host
