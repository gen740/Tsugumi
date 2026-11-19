#include "host/document_edits.hh"

#include <algorithm>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::host {
namespace {
namespace json = ::tsugumi::json;

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
// Normalize JSON before comparing: object order and whitespace are not edits.
auto Canonical(const std::string& raw) -> std::string {
  schema::Json value;
  if (auto error = schema::decode(raw, value))
    throw std::runtime_error(error->message);
  const auto text = schema::text(value.value());
  if (text.starts_with('{')) {
    auto fields = json::Object(text);
    for (auto& [key, v] : fields) {
      (void)key;
      v = Canonical(v);
    }
    return json::Encode(fields);
  }
  if (text.starts_with('[')) {
    auto values = json::Array(text);
    for (auto& v : values) v = Canonical(v);
    return json::Encode(values);
  }
  return text.starts_with('"') ? json::Quote(json::String(text)) : text;
}
}  // namespace

auto MakeSubgraph(const std::string& project,
                  const std::vector<std::string>& cards, const std::string& id)
    -> DocumentSubgraph {
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
  DocumentSubgraph result;
  result.project_json = json::Encode(outer);
  result.external_json = json::Encode(inner);
  result.link_json = json::Encode(json::Members{{"external", json::Quote(id)}});
  return result;
}
auto CopyDocuments(const std::string& state,
                   const std::vector<std::string>& paths,
                   const std::string& from, const std::string& to,
                   const std::string& name) -> DocumentCopy {
  const auto root = json::Object(state);
  const auto externals = json::Object(json::Get(root, "externals", "{}"));
  json::Members definitions, copied_externals;
  DocumentCopy result;
  const auto file = [&](const std::string& source, const std::string& target) {
    result.files.push_back({source, target});
  };
  const auto copy_external = [&](const std::string& source,
                                 const std::string& target) {
    if (!externals.contains(source))
      throw std::runtime_error("External project is missing: " + source);
    std::set<std::string> documents;
    for (const auto& [prefix, raw] : externals) {
      if (!prefix.starts_with(source)) continue;
      copied_externals[target + prefix.substr(source.size())] = raw;
      documents.insert(prefix + "tsugumi.json");
      documents.insert(prefix + "layout.json");
      for (const auto& path : paths) {
        if (!path.starts_with(prefix + "definitions/")) continue;
        const auto rest = path.substr(prefix.size() + 12);
        const auto slash = rest.find('/');
        if (slash == std::string::npos) continue;
        const auto doc = rest.substr(slash + 1);
        if (doc == "_card.json" || doc == "_meta.json" || doc == "_link.json")
          documents.insert(path);
      }
    }
    for (const auto& path : paths)
      if (path.starts_with(source) && !documents.contains(path))
        file(path, target + path.substr(source.size()));
  };
  // Paths name complete folders, never a substring of another definition.
  const auto definition_path = [](const std::string& path) {
    const auto marker = path.rfind("definitions/");
    return marker != std::string::npos &&
           (marker == 0 || path[marker - 1] == '/') && path.ends_with('/') &&
           path.find('/', marker + 12) == path.size() - 1;
  };
  if (!from.ends_with('/') || !to.ends_with('/'))
    throw std::runtime_error("Copy paths must name folders");
  if (definition_path(from) && definition_path(to)) {
    const auto f = from.rfind("definitions/"), t = to.rfind("definitions/");
    const auto source_prefix = from.substr(0, f),
               target_prefix = to.substr(0, t);
    const auto id = from.substr(f + 12, from.size() - f - 13);
    const auto new_id = to.substr(t + 12, to.size() - t - 13);
    const auto project = source_prefix.empty()
                             ? root
                             : json::Object(externals.at(source_prefix));
    auto draft = json::Object(
        json::Object(json::Get(project, "definitions", "{}")).at(id));
    auto meta = json::Object(json::Get(draft, "meta", "{}"));
    meta["name"] =
        json::Quote(name.empty() ? json::Text(meta, "name", id) : name);
    if (meta.contains("comments")) {
      auto comments = json::Array(meta.at("comments"));
      uint64_t comment_id = 0;
      for (auto& raw : comments) {
        auto comment = json::Object(raw);
        comment["id"] = json::Quote(std::to_string(++comment_id));
        raw = json::Encode(comment);
      }
      meta["comments"] = json::Encode(comments);
    }
    draft["meta"] = json::Encode(meta);
    if (draft.contains("link")) {
      const auto link = json::Object(draft.at("link"));
      const auto external = json::Text(link, "external");
      if (!external.empty()) {
        copy_external(source_prefix + "externals/" + external + "/",
                      target_prefix + "externals/" + new_id + "/");
        draft["link"] =
            json::Encode(json::Members{{"external", json::Quote(new_id)}});
      }
    }
    definitions[to] = json::Encode(draft);
    for (const auto& path : paths) {
      if (!path.starts_with(from)) continue;
      const auto rel = path.substr(from.size());
      if (rel != "_card.json" && rel != "_link.json" && rel != "_meta.json")
        file(path, to + rel);
    }
  } else {
    copy_external(from, to);
  }
  result.edits_json = json::Encode(
      json::Members{{"definitions", json::Encode(definitions)},
                    {"externals", json::Encode(copied_externals)}});
  return result;
}

auto ConnectDocuments(const std::string& state, const std::string& from_card,
                      const std::string& from_port, const std::string& to_card,
                      const std::string& to_port) -> std::string {
  auto scope = json::Object(state);
  auto project = json::Object(json::Get(scope, "project", "{}"));
  auto graph = json::Object(json::Get(project, "canvas", "{}"));
  auto definitions = json::Object(json::Get(scope, "definitions", "{}"));
  auto wires = json::Array(json::Get(graph, "connections", "[]"));
  const auto cards = json::Array(json::Get(graph, "cards", "[]"));
  const auto has_card = [&](const std::string& id) {
    return std::ranges::any_of(cards, [&](const auto& raw) {
      return json::Text(json::Object(raw), "id") == id;
    });
  };
  if (!has_card(from_card) || !has_card(to_card))
    throw std::runtime_error("A wire endpoint is missing");
  const auto is_draft = [&](const std::string& id) {
    const auto definition = json::Object(json::Get(definitions, id, "{}"));
    return json::Text(json::Object(json::Get(definition, "card", "{}")),
                      "template") == "draft";
  };
  // Draft gestures attach to the Card itself and never declare ports.
  const auto endpoint_port = [](bool draft, const std::string& port) {
    if (draft) return std::string{};
    if (port == "+")
      throw std::runtime_error("Only a Draft accepts a Card endpoint gesture");
    return port;
  };
  const auto fp = endpoint_port(is_draft(from_card), from_port);
  const bool to_draft = is_draft(to_card);
  const auto tp = endpoint_port(to_draft, to_port);
  if (!to_draft)
    std::erase_if(wires, [&](const auto& w) {
      const auto to = json::Object(json::Get(json::Object(w), "to"));
      return json::Text(to, "card") == to_card && json::Text(to, "port") == tp;
    });
  wires.push_back(json::Encode(json::Members{{"from", Endpoint(from_card, fp)},
                                             {"to", Endpoint(to_card, tp)}}));
  graph["connections"] = json::Encode(wires);
  project["canvas"] = json::Encode(graph);
  scope["project"] = json::Encode(project);
  scope.erase("ports");
  return json::Encode(scope);
}

auto DocumentsEqual(const std::string& a, const std::string& b) -> bool {
  return Canonical(a) == Canonical(b);
}

}  // namespace tsugumi::host
