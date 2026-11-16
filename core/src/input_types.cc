#include "input_types.hh"

namespace tsugumi {
auto input_types(const Resolution& graph, const CardPath& card,
                 const std::optional<std::string>& output_port)
    -> std::vector<InputEnd> {
  const auto* node = graph.find(card);
  if (!node) throw DocumentError("not_found", "unknown Card " + path_key(card));
  std::vector<InputEnd> result;
  if (output_port) {
    const auto end = graph.endpoint(card, *output_port, true);
    const auto* source = end ? &graph.nodes[end->first] : nullptr;
    const auto* type = source ? source->output(end->second) : nullptr;
    if (!type &&
        (!source || source->kind != Template::Draft || !output_port->empty()))
      throw DocumentError("not_found", "unknown output " + *output_port);
    result.push_back({"", source, end->second, type});
    return result;
  }
  auto incoming = [&](const std::string& name, size_t index,
                      const std::string& port) {
    bool connected = false;
    for (const auto& edge : graph.edges) {
      if (edge.to != index || edge.to_port != port) continue;
      const auto& source = graph.nodes[edge.from];
      // Code is generated from the destination's resolved contract, even
      // when compatible producers used different table names.
      result.push_back(
          {name, &source, edge.from_port, graph.nodes[index].input(port)});
      connected = true;
    }
    if (!connected) result.push_back({name, nullptr, "", nullptr});
  };
  if (node->kind == Template::Draft) {
    const auto index = graph.index.at(path_key(card));
    for (const auto& edge : graph.edges)
      if (edge.to == index) {
        const auto& source = graph.nodes[edge.from];
        result.push_back({edge.to_port, &source, edge.from_port,
                          source.output(edge.from_port)});
      }
  } else {
    for (const auto& input : node->inputs)
      if (auto end = graph.endpoint(card, input.name, false))
        incoming(input.name, end->first, end->second);
      else
        result.push_back({input.name, nullptr, "", nullptr});
  }
  return result;
}
}  // namespace tsugumi
