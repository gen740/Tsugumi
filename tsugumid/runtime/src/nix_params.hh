#pragma once

#include <string>
#include <string_view>

namespace nix {
class EvalState;
}

namespace tsugumi {
// NIXPATH-CONTRACT: lane B implements literal-only parsing and evaluation.
auto read_params_nix(nix::EvalState&, std::string_view text,
                     const std::string& origin) -> std::string;
}  // namespace tsugumi
