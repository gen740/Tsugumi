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

auto DecodeSourceBytes(const std::string& raw) -> std::string;
// Keep only the Cards, wires and boundary ports visible to the caller.
auto FilterProjectGraph(::tsugumi::json::Members project,
                        const std::function<bool(const std::string&)>& visible)
    -> ::tsugumi::json::Members;
auto CardStem(const std::string& name) -> std::string;
// Only definition documents and source hashes; never execution records.
struct SaveBase {
  ::tsugumi::json::Members snapshot;
  ::tsugumi::json::Members files;  // path -> JSON document value
};
using SaveRead = std::function<::tsugumi::json::Members(
    const std::string&, const ::tsugumi::json::Members&)>;
auto CaptureSaveBase(const ::tsugumi::json::Members& snapshot) -> SaveBase;
}  // namespace tsugumi::host
