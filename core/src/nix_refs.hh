#pragma once

#include <string>
#include <utility>
#include <vector>

namespace tsugumi {

/// The references `upstream.<name>`, `upstream."<path>"` and `local.<name>`
/// written in Nix code, as (local, name), in order. Comments and the text of
/// strings are skipped (an interpolation `${...}` in a string is code again),
/// so `local.conf` in a shell script or `# upstream.x` in a comment is not one;
/// nor is an attribute of something else (`pkgs.upstream.x`): only the bare
/// name or `inputs.upstream` / `inputs.local`. A quoted name with an escape or
/// an interpolation is not a plain name and is left to evaluation.
std::vector<std::pair<bool, std::string>> nix_references(const std::string& text);

}  // namespace tsugumi
