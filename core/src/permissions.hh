// What a Card may do beyond computing: reach the network, run without the
// sandbox, see folders of the host, write a database, or have Nix read a source outside nixpkgs.
// A save that widens these for any Card (a new Card counts from nothing) lists
// them, so a client can ask the user.
// And what a model's save must leave behind when it implements a Draft.
#pragma once

#include <string>
#include <vector>

#include "graph.hh"

namespace tsugumi::permissions {

// "database_write <card> <connection> <url> <table>", "network <card>", "sandbox_off <card>", "host_folder_read <card> <path>",
// "host_folder_write <card> <path>" for each widening from `before` (null:
// nothing) to `after`, in node order, after "nix_source <name> <ref>"
// for each source of a project that `before` did not have.
std::vector<std::string> widenings(const Resolution* before, const Resolution& after);

// A model implementing a Draft leaves its intent complete in _meta.json. What is
// wrong with each Draft of `before` that is no longer one in `after`, one line
// each ("<card>: <what to do>"), in node order. The readers give a file of each
// document. A save cannot rename a definition and change it too, so a Card
// keeps its path here.
std::vector<std::string> incomplete_draft_intents(const Document& before, const SourceReader& read_before,
                                                  const Document& after, const SourceReader& read_after);

}  // namespace tsugumi::permissions
