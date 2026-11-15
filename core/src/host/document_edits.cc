#include "host/document_edits.hh"

#include <algorithm>
#include <iomanip>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

#include "agent/json.hh"
#include "host/templates.hh"
#include "util/crypto.hh"

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
auto CopyDocuments(const std::string &state,
                   const std::vector<std::string> &paths,
                   const std::string &from, const std::string &to,
                   const std::string &name) -> Tsugumi::Host::DocumentCopyT {
  const auto root = json::Object(state);
  const auto externals = json::Object(json::Get(root, "externals", "{}"));
  json::Members definitions, copied_externals;
  Tsugumi::Host::DocumentCopyT result;
  const auto file = [&](const std::string &source, const std::string &target) {
    auto copy = std::make_unique<Tsugumi::Host::DocumentFileCopyT>();
    copy->from = source;
    copy->to = target;
    result.files.push_back(std::move(copy));
  };
  const auto copy_external = [&](const std::string &source,
                                 const std::string &target) {
    if (!externals.contains(source))
      throw std::runtime_error("External project is missing: " + source);
    std::set<std::string> documents;
    for (const auto &[prefix, raw] : externals) {
      if (!prefix.starts_with(source))
        continue;
      copied_externals[target + prefix.substr(source.size())] = raw;
      documents.insert(prefix + "tsugumi.json");
      documents.insert(prefix + "layout.json");
      for (const auto &path : paths) {
        if (!path.starts_with(prefix + "definitions/"))
          continue;
        const auto rest = path.substr(prefix.size() + 12);
        const auto slash = rest.find('/');
        if (slash == std::string::npos)
          continue;
        const auto doc = rest.substr(slash + 1);
        if (doc == "_card.json" || doc == "_meta.json" || doc == "_link.json")
          documents.insert(path);
      }
    }
    for (const auto &path : paths)
      if (path.starts_with(source) && !documents.contains(path))
        file(path, target + path.substr(source.size()));
  };
  // Paths name complete folders, never a substring of another definition.
  const auto definition_path = [](const std::string &path) {
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
      for (auto &raw : comments) {
        auto comment = json::Object(raw);
        comment["id"] = json::Quote(crypto::uuid4());
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
    for (const auto &path : paths) {
      if (!path.starts_with(from))
        continue;
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

auto ConnectDocuments(const std::string &state, const std::string &from_card,
                      const std::string &from_port, const std::string &to_card,
                      const std::string &to_port, const std::string &fork_id)
    -> Tsugumi::Host::DocumentConnectT {
  auto scope = json::Object(state);
  auto project = json::Object(json::Get(scope, "project", "{}"));
  auto graph = json::Object(json::Get(project, "canvas", "{}"));
  auto definitions = json::Object(json::Get(scope, "definitions", "{}"));
  auto wires = json::Array(json::Get(graph, "connections", "[]"));
  auto cards = json::Array(json::Get(graph, "cards", "[]"));
  const auto has_card = [&](const std::string &id) {
    return std::ranges::any_of(cards, [&](const auto &raw) {
      return json::Text(json::Object(raw), "id") == id;
    });
  };
  if (!has_card(from_card) || !has_card(to_card))
    throw std::runtime_error("A wire endpoint is missing");
  const auto document = [&](const std::string &id, const std::string &part) {
    return json::Object(
        json::Get(json::Object(json::Get(definitions, id, "{}")), part, "{}"));
  };
  const auto kind = [&](const std::string &id) {
    return json::Text(document(id, "card"), "template");
  };
  const auto put = [&](const std::string &id, const std::string &part,
                       const json::Members &value) {
    auto draft = json::Object(definitions.at(id));
    draft[part] = json::Encode(value);
    definitions[id] = json::Encode(draft);
  };
  const auto matches = [](const std::string &wire, const std::string &side,
                          const std::string &card, const std::string &port) {
    const auto end = json::Object(json::Get(json::Object(wire), side));
    return json::Text(end, "card") == card && json::Text(end, "port") == port;
  };
  const auto wired = [&](const std::string &id, const std::string &port,
                         bool output) {
    return std::ranges::any_of(wires, [&](const auto &wire) {
      return matches(wire, output ? "from" : "to", id, port);
    });
  };
  // Resolved port names are screen facts supplied with the scope, including
  // typed Draft outputs that have not been written into metadata.
  const auto ports = json::Object(json::Get(scope, "ports", "{}"));
  const auto draft_port = [&](const std::string &id, const std::string &port,
                              bool output, const std::string &other) {
    if (port != "+")
      return port;
    if (kind(id) != "draft")
      throw std::runtime_error("Only a Draft accepts a new port gesture");
    const auto part = output ? "meta" : "card";
    const auto list = output ? "outputs" : "inputs";
    auto doc = document(id, part);
    auto notes = json::Array(json::Get(doc, list, "[]"));
    std::vector<std::string> names;
    if (ports.contains(id))
      for (const auto &name :
           json::Array(json::Get(json::Object(ports.at(id)), list, "[]")))
        names.push_back(json::String(name));
    else
      for (const auto &note : notes)
        names.push_back(json::Text(json::Object(note), "name"));
    for (const auto &name : names)
      if (!wired(id, name, output))
        return name;
    const std::string base =
        std::regex_match(other, std::regex("[A-Za-z_][A-Za-z0-9_]*")) &&
                !std::regex_match(other, std::regex("(in|out)[0-9]*"))
            ? other
        : output ? "out"
                 : "in";
    auto name = base;
    for (size_t n = 2; std::ranges::find(names, name) != names.end(); ++n)
      name = base + std::to_string(n);
    notes.push_back(json::Encode(json::Members{{"name", json::Quote(name)}}));
    doc[list] = json::Encode(notes);
    put(id, part, doc);
    return name;
  };
  const auto fp = draft_port(from_card, from_port, true, to_port);
  const auto tp = draft_port(to_card, to_port, false, fp);
  const auto wire = [](const std::string &f, const std::string &p,
                       const std::string &to) {
    return json::Encode(json::Members{{"from", Endpoint(f, p)}, {"to", to}});
  };
  const auto branch = [&](const std::string &id, const std::string &target) {
    auto card = document(id, "card");
    auto params = json::Array(json::Get(card, "params", "[]"));
    auto branches =
        std::vector<std::string>{json::Quote("out1"), json::Quote("out2")};
    for (const auto &param : params) {
      const auto p = json::Object(param);
      if (json::Text(p, "name") == "branches")
        branches = json::Array(json::Get(p, "data", "[]"));
    }
    const auto base = target.empty() ? "out" : target;
    auto name = base;
    for (size_t n = 2;
         std::ranges::find(branches, json::Quote(name)) != branches.end(); ++n)
      name = base + "_" + std::to_string(n);
    branches.push_back(json::Quote(name));
    std::erase_if(params, [](const auto &param) {
      return json::Text(json::Object(param), "name") == "branches";
    });
    params.push_back(
        json::Encode(json::Members{{"name", json::Quote("branches")},
                                   {"type", json::Quote("string[]")},
                                   {"data", json::Encode(branches)}}));
    card["params"] = json::Encode(params);
    put(id, "card", card);
    return name;
  };
  std::vector<std::string> taken;
  for (const auto &w : wires)
    if (matches(w, "from", from_card, fp) && !matches(w, "to", to_card, tp))
      taken.push_back(w);
  auto source_card = from_card, source_port = fp;
  Tsugumi::Host::DocumentConnectT result;
  if (!taken.empty()) {
    const auto next =
        taken.size() == 1
            ? json::Text(json::Object(json::Get(json::Object(taken[0]), "to")),
                         "card")
            : "";
    if (kind(from_card) == "fork") {
      source_port = branch(from_card, to_card);
    } else if (!next.empty() && kind(next) == "fork") {
      source_card = next;
      source_port = branch(next, to_card);
    } else {
      if (fork_id.empty()) {
        result.needs_fork = true;
        return result;
      }
      if (has_card(fork_id) || definitions.contains(fork_id))
        throw std::runtime_error("A new fork Card id is needed");
      result.inserted_id = fork_id;
      cards.push_back(
          json::Encode(json::Members{{"id", json::Quote(fork_id)}}));
      auto fork = json::Object(WriteTemplate("fork", fork_id).definition_json);
      auto fork_card = json::Object(fork.at("card"));
      fork_card["params"] =
          R"([{"name":"branches","type":"string[]","data":[]}])";
      fork["card"] = json::Encode(fork_card);
      fork["meta"] = R"({"name":"fork"})";
      definitions[fork_id] = json::Encode(fork);
      auto layout = json::Object(json::Get(scope, "layout", "{}"));
      auto places = json::Object(json::Get(layout, "cards", "{}"));
      const auto position = [&](const std::string &id,
                                const std::string &axis) {
        return std::stod(
            json::Get(json::Object(json::Get(places, id, "{}")), axis, "0"));
      };
      const auto first = json::Text(
          json::Object(json::Get(json::Object(taken[0]), "to")), "card");
      json::Members point;
      for (const auto *axis : {"x", "y"}) {
        std::ostringstream number;
        number << std::setprecision(17)
               << (position(from_card, axis) + position(first, axis)) / 2;
        point[axis] = number.str();
      }
      std::erase_if(places, [&](const auto &entry) {
        return entry.first == fork_id || entry.first.starts_with(fork_id + "/");
      });
      places[fork_id] = json::Encode(point);
      layout["cards"] = json::Encode(places);
      if (layout.contains("displays")) {
        auto displays = json::Object(layout.at("displays"));
        displays.erase(fork_id);
        layout["displays"] = json::Encode(displays);
      }
      scope["layout"] = json::Encode(layout);
      std::erase_if(wires, [&](const auto &w) {
        return std::ranges::find(taken, w) != taken.end();
      });
      for (const auto &w : taken) {
        const auto to = json::Get(json::Object(w), "to");
        wires.push_back(
            wire(fork_id, branch(fork_id, json::Text(json::Object(to), "card")),
                 to));
      }
      wires.push_back(wire(from_card, fp, Endpoint(fork_id, "in")));
      source_card = fork_id;
      source_port = branch(fork_id, to_card);
    }
  }
  std::vector<std::string> replaced;
  for (const auto &w : wires)
    if (matches(w, "to", to_card, tp))
      replaced.push_back(w);
  std::erase_if(wires,
                [&](const auto &w) { return matches(w, "to", to_card, tp); });
  wires.push_back(wire(source_card, source_port, Endpoint(to_card, tp)));
  // Replacing an input also releases the old Draft output, unless the
  // external's public boundary or another wire still holds that port.
  for (const auto &w : replaced) {
    for (const auto &[side, output] :
         {std::pair{"from", true}, {"to", false}}) {
      const auto end = json::Object(json::Get(json::Object(w), side));
      const auto id = json::Text(end, "card"), port = json::Text(end, "port");
      if (!has_card(id) || kind(id) != "draft" || wired(id, port, output))
        continue;
      const auto list = output ? "outputs" : "inputs";
      const auto boundary = json::Object(json::Get(project, "boundary", "{}"));
      bool held = false;
      for (const auto &[name, ref] :
           json::Object(json::Get(boundary, list, "{}"))) {
        (void)name;
        const auto target = json::Object(ref);
        held |= json::Text(target, "card") == id &&
                json::Text(target, "port") == port;
      }
      if (held)
        continue;
      for (const auto *part : {"card", "meta"}) {
        if (output && std::string_view(part) == "card")
          continue;
        auto doc = document(id, part);
        auto notes = json::Array(json::Get(doc, list, "[]"));
        std::erase_if(notes, [&](const auto &note) {
          return json::Text(json::Object(note), "name") == port;
        });
        if (notes.empty())
          doc.erase(list);
        else
          doc[list] = json::Encode(notes);
        put(id, part, doc);
      }
    }
  }
  graph["cards"] = json::Encode(cards);
  graph["connections"] = json::Encode(wires);
  project["canvas"] = json::Encode(graph);
  scope["project"] = json::Encode(project);
  scope["definitions"] = json::Encode(definitions);
  scope.erase("ports");
  result.state_json = json::Encode(scope);
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
