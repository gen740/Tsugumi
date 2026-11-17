#pragma once
#include "tsugumi/document/graph.hh"

namespace tsugumi {
struct InputEnd {
  std::string port;
  const Node* source = nullptr;
  std::string source_port;
  const types::PortType* type = nullptr;
};
// Each incoming wire end, including distinct unnamed Draft ends. A declared
// unconnected input has no source. Boundary aliases come from resolve().
auto input_types(const Resolution& graph, const CardPath& card,
                 const std::optional<std::string>& output_port = {})
    -> std::vector<InputEnd>;
}  // namespace tsugumi
