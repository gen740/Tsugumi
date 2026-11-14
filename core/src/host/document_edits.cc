#include "host/document_edits.hh"

#include <algorithm>
#include <optional>
#include <set>
#include <stdexcept>

#include "agent/json.hh"

namespace tsugumi::host {
namespace {
namespace json = agent::json;

auto Endpoint(const std::string &card, const std::string &port) -> std::string {
  return json::Encode(
      json::Members{{"card", json::Quote(card)}, {"port", json::Quote(port)}});
}

// Reuse a boundary port when several wires name the same inner endpoint.
auto BoundaryPort(json::Members &ports, const std::string &endpoint)
    -> std::string {
  const auto end = json::Object(endpoint);
  for (const auto &[name, value] : ports)
    if (json::Text(json::Object(value), "card") == json::Text(end, "card") &&
        json::Text(json::Object(value), "port") == json::Text(end, "port"))
      return name;
  const auto stem = json::Text(end, "card") + "_" + json::Text(end, "port");
  auto name = stem;
  for (size_t i = 2; ports.contains(name); ++i)
    name = stem + std::to_string(i);
  ports[name] = endpoint;
  return name;
}
// Normalize JSON before comparing: object order and whitespace are not edits.
auto Canonical(const std::string &raw) -> std::string {
  schema::Json value;
  if (auto error = schema::decode(raw, value))
    throw std::runtime_error(error->message);
  const auto text = schema::text(value.value());
  if (text.starts_with('{')) {
    auto fields = json::Object(text);
    for (auto &[key, v] : fields) {
      (void)key;
      v = Canonical(v);
    }
    return json::Encode(fields);
  }
  if (text.starts_with('[')) {
    auto values = json::Array(text);
    for (auto &v : values)
      v = Canonical(v);
    return json::Encode(values);
  }
  return text.starts_with('"') ? json::Quote(json::String(text)) : text;
}
using Value = std::optional<std::string>;
auto Equal(const Value &a, const Value &b) -> bool {
  return a && b ? Canonical(*a) == Canonical(*b) : a == b;
}
auto OrderedMembers(const std::string &raw)
    -> std::vector<std::pair<std::string, std::string>> {
  schema::v::Mapping<schema::v::String, schema::Json> value;
  if (auto error = schema::decode(raw, value))
    throw std::runtime_error(error->message);
  std::vector<std::pair<std::string, std::string>> result;
  for (const auto &[key, v] : value.value())
    result.emplace_back(key, schema::text(v));
  return result;
}
auto Member(const json::Members &fields, const std::string &key) -> Value {
  const auto it = fields.find(key);
  return it == fields.end() ? Value{} : Value{it->second};
}
auto Path(const std::string &parent, const std::string &key) -> std::string {
  std::string escaped;
  for (char c : key)
    escaped += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
  return parent + "/" + escaped;
}
// Layout conflicts keep local placement. Project and definition conflicts
// use the existing explicit choice, including conflicts in metadata.
auto Conflict(const std::string &path, std::vector<std::string> &conflicts)
    -> void {
  if (path == "/layout" || path.starts_with("/layout/"))
    return;
  if (path.starts_with("/externals/")) {
    const auto end = path.find('/', std::string("/externals/").size());
    if (end != std::string::npos && (path.substr(end) == "/layout" ||
                                     path.substr(end).starts_with("/layout/")))
      return;
  }
  conflicts.push_back(path.empty() ? "/" : path);
}
auto Records(const std::vector<std::string> &values,
             const std::string &identity, json::Members &records,
             std::vector<std::string> &order) -> bool {
  for (const auto &v : values) {
    if (!v.starts_with('{'))
      return false;
    const auto fields = json::Object(v);
    const auto id = Member(fields, identity);
    if (!id || !id->starts_with('"'))
      return false;
    const auto key = json::Quote(json::String(*id));
    if (records.contains(key))
      return false;
    records[key] = v;
    order.push_back(key);
  }
  return true;
}
auto Merge(const Value &base, const Value &ours, const Value &theirs,
           const std::string &field, const std::string &path,
           std::vector<std::string> &conflicts) -> Value {
  if (Equal(ours, base))
    return theirs;
  if (Equal(theirs, base) || Equal(theirs, ours))
    return ours;
  if (base && ours && theirs && base->starts_with('{') &&
      ours->starts_with('{') && theirs->starts_with('{')) {
    const auto b = json::Object(*base), o = json::Object(*ours),
               t = json::Object(*theirs);
    auto order = OrderedMembers(*theirs);
    for (const auto &entry : OrderedMembers(*ours))
      if (!t.contains(entry.first))
        order.push_back(entry);
    std::string out = "{";
    for (const auto &[key, ignored] : order) {
      (void)ignored;
      const auto value = Merge(Member(b, key), Member(o, key), Member(t, key),
                               key, Path(path, key), conflicts);
      if (!value)
        continue;
      if (out.size() > 1)
        out += ',';
      out += json::Quote(key) + ':' + *value;
    }
    return out + '}';
  }
  if (base && ours && theirs && base->starts_with('[') &&
      ours->starts_with('[') && theirs->starts_with('[')) {
    const auto b = json::Array(*base), o = json::Array(*ours),
               t = json::Array(*theirs);
    const std::string identity = field == "params" || field == "inputs" ||
                                         field == "hostFolders" ||
                                         field == "nixSources"
                                     ? "name"
                                     : "id";
    json::Members bm, om, tm;
    std::vector<std::string> bi, oi, ti;
    if (Records(b, identity, bm, bi) && Records(o, identity, om, oi) &&
        Records(t, identity, tm, ti)) {
      auto order = oi == bi ? ti : oi;
      for (const auto &id : oi == bi ? oi : ti)
        if (std::ranges::find(order, id) == order.end())
          order.push_back(id);
      const auto shared = [&](const auto &ids) {
        std::vector<std::string> result;
        for (const auto &id : ids)
          if (bm.contains(id) && om.contains(id) && tm.contains(id))
            result.push_back(id);
        return result;
      };
      if (shared(oi) != shared(bi) && shared(ti) != shared(bi) &&
          shared(oi) != shared(ti))
        Conflict(path, conflicts);
      std::vector<std::string> out;
      for (const auto &id : order) {
        const auto value = Merge(Member(bm, id), Member(om, id), Member(tm, id),
                                 "", Path(path, json::String(id)), conflicts);
        if (value)
          out.push_back(*value);
      }
      return json::Encode(out);
    }
    const auto has = [](const auto &list, const auto &value) {
      return std::ranges::any_of(
          list, [&](const auto &item) { return Equal(item, value); });
    };
    const auto strings = [](const auto &list) {
      return std::ranges::all_of(
          list, [](const auto &v) { return v.starts_with('"'); });
    };
    if (field == "connections" ||
        (field == "cards" && strings(b) && strings(o) && strings(t))) {
      // Replacements of the same input are competing edits, not two new
      // independent wires. The caller must resolve them before saving.
      std::set<std::string> competing_inputs;
      if (field == "connections") {
        const auto inputs = [](const auto &wires) {
          json::Members result;
          for (const auto &w : wires) {
            const auto fields = json::Object(w);
            const auto to = Canonical(json::Get(fields, "to"));
            result[to] += Canonical(w);
          }
          return result;
        };
        const auto bw = inputs(b), ow = inputs(o), tw = inputs(t);
        auto keys = ow;
        keys.insert(tw.begin(), tw.end());
        for (const auto &[key, ignored] : keys) {
          (void)ignored;
          const auto bv = Member(bw, key), ov = Member(ow, key),
                     tv = Member(tw, key);
          if (ov != bv && tv != bv && ov != tv) {
            Conflict(path, conflicts);
            competing_inputs.insert(key);
          }
        }
      }
      std::vector<std::string> out;
      for (const auto &v : t)
        if (!has(b, v) || has(o, v))
          out.push_back(v);
      for (const auto &v : o)
        if (!has(b, v) && !has(out, v))
          out.push_back(v);
      if (!competing_inputs.empty()) {
        std::erase_if(out, [&](const auto &w) {
          return competing_inputs.contains(
              Canonical(json::Get(json::Object(w), "to")));
        });
        for (const auto &w : o)
          if (competing_inputs.contains(
                  Canonical(json::Get(json::Object(w), "to"))))
            out.push_back(w);
      }
      return json::Encode(out);
    }
  }
  Conflict(path, conflicts);
  return ours;
}
} // namespace

auto MakeSubgraph(const std::string &project,
                  const std::vector<std::string> &cards, const std::string &id)
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
  for (const auto &card : json::Array(json::Get(graph, "cards", "[]"))) {
    const auto card_id = json::Text(json::Object(card), "id");
    if (card_id == id)
      throw std::runtime_error("The new Card id is in use");
    if (selected.contains(card_id)) {
      inside.push_back(card);
      found.insert(card_id);
    } else {
      outside.push_back(card);
    }
  }
  if (found != selected)
    throw std::runtime_error("A selected Card is missing");
  json::Members inputs, outputs;
  for (const auto &raw : json::Array(json::Get(graph, "connections", "[]"))) {
    auto wire = json::Object(raw);
    const auto from = json::Object(json::Get(wire, "from"));
    const auto to = json::Object(json::Get(wire, "to"));
    const bool f = selected.contains(json::Text(from, "card"));
    const bool t = selected.contains(json::Text(to, "card"));
    if (f && t) {
      inner_wires.push_back(raw);
      continue;
    }
    if (f)
      wire["from"] = Endpoint(id, BoundaryPort(outputs, wire.at("from")));
    if (t)
      wire["to"] = Endpoint(id, BoundaryPort(inputs, wire.at("to")));
    outer_wires.push_back(json::Encode(wire));
  }
  // A scope may itself be an external. Preserve its public boundary by
  // routing references to moved Cards through the new Card as well.
  if (outer.contains("boundary")) {
    auto boundary = json::Object(outer.at("boundary"));
    for (const auto &side : {"inputs", "outputs"}) {
      auto ports = json::Object(json::Get(boundary, side, "{}"));
      for (auto &[name, raw] : ports) {
        (void)name;
        if (selected.contains(json::Text(json::Object(raw), "card")))
          raw = Endpoint(id, BoundaryPort(std::string_view(side) == "inputs"
                                              ? inputs
                                              : outputs,
                                          raw));
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
auto MergeDocuments(const std::string &base, const std::string &ours,
                    const std::string &theirs)
    -> Tsugumi::Host::DocumentMergeT {
  Tsugumi::Host::DocumentMergeT result;
  result.merged_json = *Merge(
      schema::text(schema::capture(base)), schema::text(schema::capture(ours)),
      schema::text(schema::capture(theirs)), "", "", result.conflicts);
  std::ranges::sort(result.conflicts);
  result.conflicts.erase(
      std::unique(result.conflicts.begin(), result.conflicts.end()),
      result.conflicts.end());
  return result;
}
} // namespace tsugumi::host
