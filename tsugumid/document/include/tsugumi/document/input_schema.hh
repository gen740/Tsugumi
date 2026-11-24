#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace tsugumi {
struct Node;
struct Resolution;

namespace types {
struct Type;

struct InputDeclarations {
  std::map<std::string, std::string> types;
  std::vector<std::string> order;
  std::set<std::string> attributes;
  bool artifact = false;
};

// Adds a candidate and its reachable declarations under the port namespace.
auto render_input_candidate(const Type& type, const std::string& prefix,
                            const std::string& schema_id,
                            InputDeclarations& declarations) -> std::string;

}  // namespace types

// Inputs is empty when no connected input has a known type. An error
// (e.g. conflicting candidate declarations) is a definition diagnostic.
auto generated_input_schema(const Resolution& graph, const Node& node)
    -> std::string;

}  // namespace tsugumi
