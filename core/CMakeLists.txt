include(cmake/headers.cmake)
# Component fragments run in this directory scope; paths and target ownership
# remain unchanged until the corresponding separation commit.
# Values fixed at build time (Nix supplies them for the container image).
set(TSUGUMI_CARD_NIXPKGS_REV "$ENV{TSUGUMI_CARD_NIXPKGS_REV}" CACHE STRING "nixpkgs revision for Cards")
set(TSUGUMI_CARD_NIXPKGS_NARHASH "$ENV{TSUGUMI_CARD_NIXPKGS_NARHASH}" CACHE STRING "nixpkgs NAR hash for Cards")

include(cmake/json.cmake)
include(cmake/transport.cmake)
include(cmake/nix.cmake)
include(cmake/document.cmake)
include(cmake/daemon.cmake)
include(cmake/host.cmake)
include(cmake/test-support.cmake)
include(cmake/vm.cmake)
