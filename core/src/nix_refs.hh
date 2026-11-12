#pragma once

#include <string>
#include <vector>

namespace tsugumi {

// Direct inputs.<port> references in Nix code, skipping comments, literal
// strings and the SDK's own attributes. Interpolations are code again.
auto nix_references(const std::string& text) -> std::vector<std::string>;

}  // namespace tsugumi
