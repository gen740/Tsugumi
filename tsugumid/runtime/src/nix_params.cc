#include "nix_params.hh"

#include <stdexcept>

namespace tsugumi {
// NIXPATH-CONTRACT: replaced by lane B's AST validator.
auto read_params_nix(nix::EvalState&, std::string_view,
                     const std::string& origin) -> std::string {
  throw std::runtime_error(origin + ": params.nix parsing requires lane B");
}
}  // namespace tsugumi
