// What a model reads of a tool reply. Receipts and the UI keep the complete
// reply; these views shape it for a model's context.
#pragma once

#include <string>
#include <string_view>

#include "tsugumi/util/json_object.hh"

namespace tsugumi::agent {

/** Recorded values decoded from their FlatBuffers schema into JSON (value),
 *  and binary schemas dropped from ports, anywhere in a reply. */
auto DecodeRecordedValues(std::string reply) -> std::string;

/** A project snapshot as Cards, wires, files, settings, diagnostics and the
 *  last run. */
auto InspectSummary(const json::Members& snapshot) -> json::Members;

/** A SaveResult as what changed, the diagnostics and the changed Cards'
 *  resolved ports, without the echoed documents. */
auto SaveSummary(const json::Members& save) -> json::Members;

/** The model's view of a tool's reply text. arguments are the resolved
 *  call's arguments. */
auto ModelView(std::string_view tool, const json::Members& arguments,
               std::string text) -> std::string;

}  // namespace tsugumi::agent
