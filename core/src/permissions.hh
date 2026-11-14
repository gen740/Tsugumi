// What a Card may do beyond computing: reach the network, run without the
// sandbox, see folders of the host, have Nix read a source outside nixpkgs.
// A save that widens these for any Card (a new Card counts from nothing) lists
// them, so a client can ask the user.
#pragma once

#include <string>
#include <vector>

#include "graph.hh"

namespace tsugumi::permissions {

// "network <card>", "sandbox_off <card>", "host_folder_read <card> <path>",
// "host_folder_write <card> <path>" for each widening from `before` (null:
// nothing) to `after`, in node order, after "nix_source <name> <ref>"
// for each source of a project that `before` did not have.
std::vector<std::string> widenings(const Resolution* before, const Resolution& after);

}  // namespace tsugumi::permissions
