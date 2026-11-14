#include "host/document_edits.hh"

#include <set>
#include <stdexcept>

#include "agent/json.hh"

namespace tsugumi::host {
namespace {
namespace json = agent::json;

auto Endpoint(const std::string& card, const std::string& port) -> std::string {
  return json::Encode(
      json::Members{{"card", json::Quote(card)}, {"port", json::Quote(port)}});
}

// Reuse a boundary port when several wires name the same inner endpoint.
auto BoundaryPort(json::Members& ports, const std::string& endpoint)
    -> std::string {
  const auto end = json::Object(endpoint);
  for (const auto& [name, value] : ports)
    if (json::Text(json::Object(value), "card") == json::Text(end, "card") &&
        json::Text(json::Object(value), "port") == json::Text(end, "port"))
      return name;
  const auto stem = json::Text(end, "card") + "_" + json::Text(end, "port");
  auto name = stem;
  for (size_t i = 2; ports.contains(name); ++i) name = stem + std::to_string(i);
  ports[name] = endpoint;
  return name;
}
}  // namespace

auto MakeSubgraph(const std::string& project,
                  const std::vector<std::string>& cards, const std::string& id)
    -> Tsugumi::Host::DocumentSubgraphT {
  const std::set<std::string> selected(cards.begin(), cards.end());
  if (selected.empty() || id.empty() || id.find('/') != std::string::npos ||
      id == "." || id == "..")
    throw std::runtime_error("Choose Cards and a new Card id");
  auto outer = json::Object(project);
  // Carry all project conditions, including pinned Nix sources and future
  // settings. Only the graph, name and boundary change in the new project.
  auto inner = outer;
  auto graph = json::Object(json::Get(outer, "canvas", "{}"));
  auto inner_graph = graph;
  std::vector<std::string> inside, outside, inner_wires, outer_wires;
  std::set<std::string> found;
  for (const auto& card : json::Array(json::Get(graph, "cards", "[]"))) {
    const auto card_id = json::Text(json::Object(card), "id");
    if (card_id == id) throw std::runtime_error("The new Card id is in use");
    if (selected.contains(card_id)) {
      inside.push_back(card);
      found.insert(card_id);
    } else {
      outside.push_back(card);
    }
  }
  if (found != selected) throw std::runtime_error("A selected Card is missing");
  json::Members inputs, outputs;
  for (const auto& raw : json::Array(json::Get(graph, "connections", "[]"))) {
    auto wire = json::Object(raw);
    const auto from = json::Object(json::Get(wire, "from"));
    const auto to = json::Object(json::Get(wire, "to"));
    const bool f = selected.contains(json::Text(from, "card"));
    const bool t = selected.contains(json::Text(to, "card"));
    if (f && t) {
      inner_wires.push_back(raw);
      continue;
    }
    if (f) wire["from"] = Endpoint(id, BoundaryPort(outputs, wire.at("from")));
    if (t) wire["to"] = Endpoint(id, BoundaryPort(inputs, wire.at("to")));
    outer_wires.push_back(json::Encode(wire));
  }
  // A scope may itself be an external. Preserve its public boundary by
  // routing references to moved Cards through the new Card as well.
  if (outer.contains("boundary")) {
    auto boundary = json::Object(outer.at("boundary"));
    for (const auto& side : {"inputs", "outputs"}) {
      auto ports = json::Object(json::Get(boundary, side, "{}"));
      for (auto& [name, raw] : ports) {
        (void)name;
        if (selected.contains(json::Text(json::Object(raw), "card")))
          raw = Endpoint(
              id,
              BoundaryPort(
                  std::string_view(side) == "inputs" ? inputs : outputs, raw));
      }
      boundary[side] = json::Encode(ports);
    }
    outer["boundary"] = json::Encode(boundary);
  }
  outside.push_back(json::Encode(json::Members{{"id", json::Quote(id)}}));
  graph["cards"] = json::Encode(outside);
  graph["connections"] = json::Encode(outer_wires);
  outer["canvas"] = json::Encode(graph);
  inner_graph["cards"] = json::Encode(inside);
  inner_graph["connections"] = json::Encode(inner_wires);
  inner["canvas"] = json::Encode(inner_graph);
  inner["name"] = json::Quote(id);
  inner["boundary"] = json::Encode(json::Members{
      {"inputs", json::Encode(inputs)}, {"outputs", json::Encode(outputs)}});
  Tsugumi::Host::DocumentSubgraphT result;
  result.project_json = json::Encode(outer);
  result.external_json = json::Encode(inner);
  result.link_json = json::Encode(json::Members{{"external", json::Quote(id)}});
  return result;
}
}  // namespace tsugumi::host
