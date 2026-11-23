#pragma once

#include <string>
#include <string_view>

namespace nix {
class EvalState;
}

namespace tsugumi {
auto read_params_nix(nix::EvalState& state, std::string_view text,
                     const std::string& origin) -> std::string;
// NIXPATH-CONTRACT: standalone adapter for lane B before lane A is merged.
auto evaluate_literal_params(const std::string& def_dir, std::string_view text)
    -> std::string;
}  // namespace tsugumi
