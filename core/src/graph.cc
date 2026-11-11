#include "graph.hh"
#include "nix_refs.hh"

#include <map>
#include <mutex>

#include <deque>
#include <limits>
#include <set>
#include <tuple>

namespace tsugumi {

using types::PortKind;
using types::PortType;
using types::TypeRef;

const char* template_name(Template t) {
  switch (t) {
    case Template::Code: return "none";
    case Template::Trigger: return "trigger";
    case Template::Input: return "input";
    case Template::Fork: return "fork";
    case Template::Branch: return "branch";
    case Template::Merge: return "merge";
    case Template::Logger: return "logger";
    case Template::Subgraph: return "subgraph";
    case Template::Draft: return "draft";
  }
  return "none";
}

std::optional<ParamType> parse_param_type(const std::string& s) {
  static const std::map<std::string, ParamType> m = {
      {"bool", ParamType::Bool},          {"int64", ParamType::Int64},
      {"uint64", ParamType::UInt64},      {"double", ParamType::Double},
      {"string", ParamType::String},      {"bool[]", ParamType::BoolArray},
      {"int64[]", ParamType::Int64Array}, {"uint64[]", ParamType::UInt64Array},
      {"double[]", ParamType::DoubleArray}, {"string[]", ParamType::StringArray},
  };
  auto it = m.find(s);
  if (it == m.end()) return std::nullopt;
  return it->second;
}

const char* param_type_name(ParamType t) {
  switch (t) {
    case ParamType::Bool: return "bool";
    case ParamType::Int64: return "int64";
    case ParamType::UInt64: return "uint64";
    case ParamType::Double: return "double";
    case ParamType::String: return "string";
    case ParamType::BoolArray: return "bool[]";
    case ParamType::Int64Array: return "int64[]";
    case ParamType::UInt64Array: return "uint64[]";
    case ParamType::DoubleArray: return "double[]";
    case ParamType::StringArray: return "string[]";
  }
  return "string";
}

std::string param_fbs_type(ParamType t) {
  switch (t) {
    case ParamType::Bool: return "bool";
    case ParamType::Int64: return "long";
    case ParamType::UInt64: return "ulong";
    case ParamType::Double: return "double";
    case ParamType::String: return "string";
    case ParamType::BoolArray: return "[bool]";
    case ParamType::Int64Array: return "[long]";
    case ParamType::UInt64Array: return "[ulong]";
    case ParamType::DoubleArray: return "[double]";
    case ParamType::StringArray: return "[string]";
  }
  return "string";
}

namespace {

template <class E>
bool decodes_as(const hosq::json::GenericDom<>& v) {
  E e;
  return !schema::decode(v, e);
}

}  // namespace

std::optional<std::string> check_param_value(ParamType t, const hosq::json::GenericDom<>& v) {
  namespace sv = schema::v;
  bool ok = false;
  switch (t) {
    case ParamType::Bool: ok = decodes_as<sv::Bool>(v); break;
    case ParamType::Int64: ok = decodes_as<sv::Int64>(v); break;
    case ParamType::UInt64: ok = decodes_as<sv::Uint64>(v); break;
    case ParamType::Double: ok = decodes_as<schema::Number>(v); break;
    case ParamType::String: ok = decodes_as<sv::String>(v); break;
    case ParamType::BoolArray: ok = decodes_as<sv::Array<sv::Bool>>(v); break;
    case ParamType::Int64Array: ok = decodes_as<sv::Array<sv::Int64>>(v); break;
    case ParamType::UInt64Array: ok = decodes_as<sv::Array<sv::Uint64>>(v); break;
    case ParamType::DoubleArray: ok = decodes_as<sv::Array<schema::Number>>(v); break;
    case ParamType::StringArray: ok = decodes_as<sv::Array<sv::String>>(v); break;
  }
  if (ok) return std::nullopt;
  return std::string("expected ") + param_type_name(t) + " (integers must be in range, no implicit conversion)";
}

Template template_of(const DefinitionDoc* def) {
  if (!def) return Template::Draft;
  if (def->link) return Template::Subgraph;
  const auto* t = schema::get(def->card.template_name);
  if (!t || *t == "none" || t->empty()) return Template::Code;
  static const std::map<std::string, Template> m = {
      {"trigger", Template::Trigger}, {"input", Template::Input},   {"fork", Template::Fork},
      {"branch", Template::Branch},   {"merge", Template::Merge},   {"logger", Template::Logger},
      {"draft", Template::Draft},
  };
  auto it = m.find(*t);
  return it == m.end() ? Template::Code : it->second;
}

std::vector<std::string> string_array_param(const hosq::json::GenericDom<>& v) {
  schema::v::Array<schema::v::String> a;
  if (schema::decode(v, a)) return {};
  return a.value();
}

const PortType* Node::input(const std::string& n) const {
  for (const auto& p : inputs)
    if (p.name == n) return &p;
  return nullptr;
}

const PortType* Node::output(const std::string& n) const {
  for (const auto& p : outputs)
    if (p.name == n) return &p;
  return nullptr;
}

const Node* Resolution::find(const CardPath& p) const {
  auto it = index.find(path_key(p));
  return it == index.end() ? nullptr : &nodes[it->second];
}

std::vector<Diag> Resolution::all_diagnostics(bool executable) const {
  std::vector<Diag> out = diags;
  for (const auto& n : nodes) {
    out.insert(out.end(), n.diags.begin(), n.diags.end());
    if (executable) out.insert(out.end(), n.exec_diags.begin(), n.exec_diags.end());
  }
  return out;
}

namespace {

Diag diag(const CardPath& card, std::string code, std::string message, bool error = true,
          std::optional<std::string> path = std::nullopt) {
  Diag d;
  d.error = error;
  d.code = std::move(code);
  d.message = std::move(message);
  d.card = card;
  d.path = std::move(path);
  return d;
}

class Builder {
 public:
  Builder(const Document& doc, const SourceReader& read) : doc_(doc), read_(read) {}

  std::shared_ptr<Resolution> run() {
    res_ = std::make_shared<Resolution>();
    scope(doc_.project, "", {}, 0);
    connect_all();
    resolve_types();
    finish_checks();
    compute_guarantees();
    check_nix_references();
    return res_;
  }

 private:
  struct RawEdge {
    CardPath scope;
    std::string from;
    std::string from_port;
    std::string to;
    std::string to_port;
  };

  void timeout(Node& n, const char* key, const hosq::json::GenericDom<>* v, std::optional<uint64_t>& out) {
    if (!v) return;
    if (schema::is_null(*v)) {
      out.reset();  // explicit null: no deadline
      return;
    }
    schema::v::Uint64 u;
    if (!schema::decode(*v, u) && u.value() > 0) out = u.value();
    else n.diags.push_back(diag(n.path, "invalid_setting", std::string(key) + " must be a positive integer or null"));
  }

  void read_settings(Node& n, const DefinitionDoc* def) {
    auto& s = n.settings;
    s.name = n.path.back();
    if (!def || def->link) return;
    const auto& d = def->card;
    auto str = [&](const auto& f, std::string& out) {
      if (const auto* v = schema::get(f)) out = *v;
    };
    auto boolean = [&](const auto& f, bool& out) {
      if (const auto* v = schema::get(f)) out = *v;
    };
    str(d.symbol, s.symbol);
    if (const auto* lang = schema::get(d.language)) s.language = *lang == "none" ? "" : *lang;
    if (const auto* t = schema::get(d.trigger)) {
      if (*t == "allInputs" || *t == "eachInput") s.any_mode = *t == "eachInput";
      else n.diags.push_back(diag(n.path, "invalid_setting", "trigger must be allInputs or eachInput"));
    }
    if (const auto* ins = schema::get(d.inputs)) {
      std::set<std::string> seen;
      for (const auto& in : *ins) {
        const auto* name = schema::get(in.name);
        if (!name || !valid_id(*name)) {
          n.diags.push_back(diag(n.path, "invalid_setting", "inputs entries need a valid name"));
          continue;
        }
        if (!seen.insert(*name).second) n.diags.push_back(diag(n.path, "invalid_setting", "duplicate input " + *name));
        const auto* opt = schema::get(in.optional);
        n.input_defs.push_back({*name, opt && *opt});
      }
    }
    if (const auto* v = schema::get(d.max_runs)) {
      if (*v > 0) s.max_runs = *v;
      else n.diags.push_back(diag(n.path, "invalid_setting", "maxRuns must be a positive integer (0 is rejected)"));
    }
    timeout(n, "timeoutSeconds", schema::get(d.timeout_seconds), s.timeout_seconds);
    timeout(n, "buildTimeoutSeconds", schema::get(d.build_timeout_seconds), s.build_timeout_seconds);
    boolean(d.record_io, s.record_io);
    boolean(d.retain_artifact, s.retain_artifact);
    boolean(d.filesystem, s.filesystem);
    boolean(d.sandbox, s.sandbox);
    boolean(d.reuse_worker, s.reuse_worker);
    boolean(d.network, s.network);
    boolean(d.markdown, s.markdown);
    if (const auto* v = schema::get(d.system)) {
      if (*v == "x86_64-linux" || *v == "aarch64-linux") s.system = *v == native_system() ? "" : *v;
      else n.diags.push_back(diag(n.path, "invalid_setting", "system must be x86_64-linux or aarch64-linux"));
    }
    if (const auto* v = schema::get(d.artifact_output)) s.artifact_output = *v;
    if (const auto* folders = schema::get(d.host_folders)) {
      std::set<std::string> names;
      for (const auto& f : *folders) {
        const auto* path = schema::get(f.path);
        const auto* name = schema::get(f.name);
        if (!path || path->empty() || path->front() != '/' || !name || !valid_id(*name)) {
          n.diags.push_back(diag(n.path, "invalid_setting", "hostFolders entries need an absolute path and a name"));
          continue;
        }
        if (!names.insert(*name).second) {
          n.diags.push_back(diag(n.path, "invalid_setting", "duplicate host folder name " + *name));
          continue;
        }
        const auto* w = schema::get(f.write);
        s.host_folders.push_back({*path, *name, w && *w});
      }
    }
    read_params(n, def);
  }

  void read_params(Node& n, const DefinitionDoc* def) {
    std::set<std::string> names;
    if (const auto* list = schema::get(def->card.params)) {
      for (const auto& p : *list) {
        const auto* name = schema::get(p.name);
        const auto* type = schema::get(p.type);
        if (!name || !valid_id(*name) || !type) {
          n.diags.push_back(diag(n.path, "invalid_param", "params need a name and type"));
          continue;
        }
        auto pt = parse_param_type(*type);
        if (!pt) {
          n.diags.push_back(diag(n.path, "invalid_param", "param " + *name + " has unknown type " + *type));
          continue;
        }
        if (!names.insert(*name).second) {
          n.diags.push_back(diag(n.path, "invalid_param", "duplicate param " + *name));
          continue;
        }
        ParamDef d;
        d.name = *name;
        d.type = *pt;
        if (const auto* nix = schema::get(p.nix)) d.nix = *nix;
        if (const auto* data = schema::get(p.data)) {
          if (auto err = check_param_value(d.type, *data))
            n.diags.push_back(diag(n.path, "invalid_param", "param " + d.name + ": " + *err));
          else d.data = *data;
        }
        n.settings.params.push_back(std::move(d));
      }
    }
    for (const auto& p : n.settings.params)
      if (!p.data) n.exec_diags.push_back(diag(n.path, "missing_param", "param " + p.name + " has no value"));
  }

  const ParamDef* param(const Node& n, const std::string& name) {
    for (const auto& p : n.settings.params)
      if (p.name == name) return &p;
    return nullptr;
  }

  void load_catalog(Node& n) {
    std::string rel = n.def_dir + "_outputs.fbs";
    if (!doc_.sources.count(rel)) {
      if (n.kind == Template::Code || n.kind == Template::Input)
        n.exec_diags.push_back(diag(n.path, "missing_outputs", rel + " is missing", true, rel));
      return;
    }
    // Parsed catalogs by content (the source's hash): an unchanged
    // _outputs.fbs is neither read nor parsed again.
    std::string key = rel + '\0' + doc_.sources.at(rel).hash;
    static std::mutex mu;
    static std::map<std::string, types::CatalogResult> parsed;
    std::optional<types::CatalogResult> cat;
    {
      std::lock_guard<std::mutex> l(mu);
      if (auto it = parsed.find(key); it != parsed.end()) cat = it->second;
    }
    if (!cat) {
      auto text = read_(rel);
      if (!text) {
        n.diags.push_back(diag(n.path, "external_change", rel + " could not be read", true, rel));
        return;
      }
      cat = types::parse_catalog(*text, rel);
      std::lock_guard<std::mutex> l(mu);
      if (parsed.size() >= 2048) parsed.clear();
      parsed[key] = *cat;
    }
    for (const auto& e : cat->errors) n.diags.push_back(diag(n.path, "invalid_outputs", e, true, rel));
    n.outputs = cat->ports;
  }

  // The definition of a Card written in an external project (read as sources).
  const DefinitionDoc* external_definition(Node& n) {
    auto card = read_(n.def_dir + "_card.json");
    auto link = read_(n.def_dir + "_link.json");
    if (card.has_value() == link.has_value()) {
      n.diags.push_back(diag(n.path, "invalid_document", n.def_dir + " needs _card.json or _link.json (one of them)"));
      return nullptr;
    }
    DefinitionDoc d;
    const std::string& id = n.path.back();
    try {
      if (card) {
        d.card = parse_card_json(id, *card);
        d.card_json = *card;
      } else {
        d.link = parse_link_json(id, *link).external->value();
        d.link_json = *link;
      }
      if (auto meta = read_(n.def_dir + "_meta.json")) {
        d.meta = parse_meta_json(*meta);
        d.meta_json = *meta;
      }
    } catch (const DocumentError& e) {
      n.diags.push_back(diag(n.path, "invalid_document", e.what(), true, n.def_dir));
      return nullptr;
    }
    external_defs_.push_back(std::move(d));
    return &external_defs_.back();
  }

  // One project's canvas: the root project (prefix "") or an external
  // ("externals/<p>/", nested further inside its own externals).
  void scope(const schema::Project& project, const std::string& prefix, const CardPath& scope_path, int depth) {
    std::string pkgs = project.pkgs_path->value_or("tsugumi:default");
    std::vector<NixSourceRef> sources;
    if (project.nix_sources->has_value())
      for (const auto& s : project.nix_sources->value())
        sources.push_back({s.name->value(), s.type->value(), s.ref->value()});
    for (const auto& card : project.canvas->value().cards->value()) {
      Node n;
      n.path = scope_path;
      n.path.push_back(card.id->value());
      n.def_dir = prefix + "definitions/" + card.id->value() + "/";
      n.project_dir = prefix;
      n.pkgs_path = pkgs;
      n.nix_sources = sources;
      const DefinitionDoc* def = nullptr;
      if (prefix.empty()) {
        auto it = doc_.definitions.find(card.id->value());
        if (it != doc_.definitions.end()) def = &it->second;
      } else {
        def = external_definition(n);
      }
      n.kind = template_of(def);
      read_settings(n, def);
      switch (n.kind) {
        case Template::Code:
          if (n.settings.language != "cpp" && n.settings.language != "python" &&
              n.settings.language != "bash" && n.settings.language != "r" && n.settings.language != "julia")
            n.exec_diags.push_back(diag(n.path, "invalid_language",
                                        "language must be cpp, python, bash, r or julia"));
          if (!doc_.sources.count(n.def_dir + "default.nix"))
            n.exec_diags.push_back(diag(n.path, "missing_nix", "default.nix is missing", true,
                                        n.def_dir + "default.nix"));
          load_catalog(n);
          break;
        case Template::Input:
          // Optional start input: connected, the value is produced when the
          // start signal arrives (e.g. from the trigger); unconnected, the
          // Card is a source that fires once per run.
          n.input_defs = {{"start", true}};
          load_catalog(n);
          break;
        case Template::Trigger:
          n.outputs.push_back({"out", PortKind::FlatBuffer, false, types::empty_type()});
          break;
        case Template::Fork: {
          n.input_defs = {{"in", false}};
          std::vector<std::string> branches = {"out1", "out2"};
          if (const auto* p = param(n, "branches"); p && p->data && p->type == ParamType::StringArray) {
            branches.clear();
            for (const auto& b : string_array_param(*p->data))
              if (valid_id(b)) branches.push_back(b);
              else n.diags.push_back(diag(n.path, "invalid_param", "fork branches must be ids"));
          }
          for (const auto& b : branches) n.outputs.push_back({b, PortKind::FlatBuffer, false, nullptr});
          break;
        }
        case Template::Branch:
          n.input_defs = {{"value", false}, {"condition", false}};
          n.outputs = {{"a", PortKind::FlatBuffer, true, nullptr}, {"b", PortKind::FlatBuffer, true, nullptr}};
          break;
        case Template::Merge:
          n.settings.any_mode = true;
          n.outputs = {{"out", PortKind::FlatBuffer, false, nullptr}};
          if (n.input_defs.empty())
            n.diags.push_back(diag(n.path, "invalid_setting", "merge needs declared inputs"));
          for (auto& in : n.input_defs) in.optional = true;
          break;
        case Template::Logger:
          n.input_defs = {{"in", false}};
          break;
        case Template::Draft:
          // A Draft can be wired before it is implemented: its outputs are the
          // typed ones of _outputs.fbs and the names in _meta.json (a wire drawn
          // from the Draft adds one there), carrying an empty message until
          // implemented.
          load_catalog(n);
          if (def && def->meta)
            if (const auto* outs = schema::get(def->meta->outputs))
              for (const auto& o : *outs)
                if (const auto* name = schema::get(o.name); name && valid_id(*name) && !n.output(*name))
                  n.outputs.push_back({*name, PortKind::FlatBuffer, true, types::empty_type()});
          n.exec_diags.push_back(diag(n.path, "draft", "draft cards are not implemented and cannot run"));
          break;
        case Template::Subgraph:
          n.is_virtual = true;
          break;
      }
      for (const auto& in : n.input_defs)
        n.inputs.push_back({in.name, PortKind::FlatBuffer, in.optional, nullptr});
      if (n.settings.retain_artifact) {
        if (!n.settings.artifact_output)
          n.diags.push_back(diag(n.path, "invalid_setting", "retainArtifact needs artifactOutput"));
      }
      size_t idx = res_->nodes.size();
      res_->index[path_key(n.path)] = idx;
      CardPath inner_scope = n.path;
      std::optional<std::string> link = def ? def->link : std::nullopt;
      res_->nodes.push_back(std::move(n));

      if (link) {
        // A Card in Card: the project externals/<link>/ of this project.
        std::string ext = prefix + "externals/" + *link + "/";
        res_->nodes[idx].external = ext;
        auto text = read_(ext + "tsugumi.json");
        if (!text) {
          res_->nodes[idx].diags.push_back(diag(inner_scope, "unknown_external", ext + " is not a Tsugumi project (no tsugumi.json)", true, ext));
        } else if (depth >= 15) {
          res_->nodes[idx].diags.push_back(diag(inner_scope, "invalid_graph", "Card in Card nesting deeper than 15", true, ext));
        } else {
          try {
            auto inner = parse_external_project(ext + "tsugumi.json", *text);
            if (const auto* b = schema::get(inner.boundary)) res_->nodes[idx].boundary = *b;
            projects_.push_back(std::move(inner));
            scope(projects_.back(), ext, inner_scope, depth + 1);
          } catch (const DocumentError& e) {
            res_->nodes[idx].diags.push_back(diag(inner_scope, "invalid_graph", e.what(), true, ext + "tsugumi.json"));
          }
        }
      }
    }
    for (const auto& c : project.canvas->value().connections->value()) {
      RawEdge e;
      e.scope = scope_path;
      e.from = c.from->value().card->value();
      e.from_port = c.from->value().port->value();
      e.to = c.to->value().card->value();
      e.to_port = c.to->value().port->value();
      raw_.push_back(std::move(e));
    }
  }

  // Resolves an endpoint through subgraph boundaries down to a leaf node.
  std::optional<std::pair<size_t, std::string>> leaf(CardPath path, std::string port, bool output,
                                                     const CardPath& report_on) {
    for (int guard = 0; guard < 32; ++guard) {
      auto it = res_->index.find(path_key(path));
      if (it == res_->index.end()) return std::nullopt;
      Node& n = res_->nodes[it->second];
      if (n.kind != Template::Subgraph) return std::make_pair(it->second, port);
      const auto* b = n.boundary ? &*n.boundary : nullptr;
      const auto* side = b ? (output ? schema::get(b->outputs) : schema::get(b->inputs)) : nullptr;
      const schema::Endpoint* ep = nullptr;
      if (side)
        for (const auto& [name, e] : side->entries())
          if (name == port) ep = &e;
      if (!ep) {
        n.diags.push_back(diag(n.path, "unknown_port",
                               std::string("subgraph has no boundary ") + (output ? "output " : "input ") + port));
        return std::nullopt;
      }
      path.push_back(ep->card->value());
      port = ep->port->value();
      if (!res_->index.count(path_key(path))) {
        n.diags.push_back(diag(n.path, "unknown_card",
                               "boundary " + port + " points to unknown card " + path_key(path)));
        return std::nullopt;
      }
    }
    res_->diags.push_back(diag(report_on, "invalid_graph", "subgraph boundary nesting too deep"));
    return std::nullopt;
  }

  void connect_all() {
    // Boundary ports become ports of the virtual subgraph node for display.
    for (auto& n : res_->nodes) {
      if (n.kind != Template::Subgraph) continue;
      if (const auto* b = n.boundary ? &*n.boundary : nullptr) {
        if (const auto* ins = schema::get(b->inputs))
          for (const auto& [name, _] : ins->entries()) n.inputs.push_back({name, PortKind::FlatBuffer, true, nullptr});
        if (const auto* outs = schema::get(b->outputs))
          for (const auto& [name, _] : outs->entries()) n.outputs.push_back({name, PortKind::FlatBuffer, true, nullptr});
      }
    }
    for (const auto& e : raw_) {
      CardPath from = e.scope;
      CardPath to = e.scope;
      from.push_back(e.from);
      to.push_back(e.to);
      auto src = leaf(from, e.from_port, true, from);
      auto dst = leaf(to, e.to_port, false, to);
      if (!src || !dst) continue;
      Node& sn = res_->nodes[src->first];
      Node& dn = res_->nodes[dst->first];
      bool ok = true;
      if (!sn.output(src->second)) {
        sn.diags.push_back(diag(sn.path, "unknown_port", "no output port " + src->second));
        ok = false;
      }
      if (!dn.input(dst->second)) {
        dn.diags.push_back(diag(dn.path, "unknown_port", "no input port " + dst->second));
        ok = false;
      }
      if (ok) res_->edges.push_back({src->first, src->second, dst->first, dst->second});
    }
    // One output feeds one wire: a value goes to several Cards through a
    // fork Card (one way to write it, drawn as a hub on the canvas).
    std::map<std::tuple<CardPath, std::string, std::string>, size_t> wires;
    for (const auto& e : raw_) ++wires[{e.scope, e.from, e.from_port}];
    for (const auto& [k, count] : wires) {
      if (count < 2) continue;
      CardPath from = std::get<0>(k);
      from.push_back(std::get<1>(k));
      std::string msg = "output " + std::get<2>(k) + " has " + std::to_string(count) +
                        " wires; an output feeds one input (send a value to several Cards through a fork Card)";
      auto it = res_->index.find(path_key(from));
      if (it != res_->index.end()) res_->nodes[it->second].diags.push_back(diag(from, "fan_out", msg));
      else res_->diags.push_back(diag(from, "fan_out", msg));
    }
  }

  types::PortType* in_port(Node& n, const std::string& name) {
    for (auto& p : n.inputs)
      if (p.name == name) return &p;
    return nullptr;
  }
  types::PortType* out_port(Node& n, const std::string& name) {
    for (auto& p : n.outputs)
      if (p.name == name) return &p;
    return nullptr;
  }

  void resolve_types() {
    std::set<std::pair<size_t, std::string>> mismatch_reported;
    for (size_t pass = 0; pass <= res_->nodes.size() + 1; ++pass) {
      bool changed = false;
      for (const auto& e : res_->edges) {
        auto* src = out_port(res_->nodes[e.from], e.from_port);
        auto* dst = in_port(res_->nodes[e.to], e.to_port);
        if (!src || !dst || !src->type) continue;
        if (!dst->type) {
          dst->type = src->type;
          dst->kind = src->kind;
          changed = true;
        } else if (dst->type != src->type && !types::compatible(*dst->type, *src->type) &&
                   mismatch_reported.insert({e.to, e.to_port}).second) {
          auto& dn = res_->nodes[e.to];
          dn.diags.push_back(diag(dn.path, "type_mismatch",
                                  "input " + e.to_port + " receives incompatible types " +
                                      dst->type->root_type + " and " + src->type->root_type));
        }
      }
      for (auto& n : res_->nodes) changed |= derive(n);
      if (!changed) break;
    }
    for (auto& n : res_->nodes) {
      if (n.is_virtual) continue;
      for (auto& p : n.outputs)
        if (!p.type && n.kind != Template::Code && n.kind != Template::Input && connected_out(n, p.name))
          n.diags.push_back(diag(n.path, "unresolved_type",
                                 "output " + p.name +
                                     " type cannot be derived (unresolved cycle or missing input)"));
    }
  }

  bool connected_out(const Node& n, const std::string& port) {
    size_t idx = res_->index.at(path_key(n.path));
    for (const auto& e : res_->edges)
      if (e.from == idx && e.from_port == port) return true;
    return false;
  }

  bool derive(Node& n) {
    bool changed = false;
    switch (n.kind) {
      case Template::Fork: {
        const auto* in = n.input("in");
        if (in && in->type)
          for (auto& o : n.outputs)
            if (!o.type) { o.type = in->type; o.kind = in->kind; changed = true; }
        break;
      }
      case Template::Branch: {
        const auto* v = n.input("value");
        if (v && v->type)
          for (auto& o : n.outputs)
            if (!o.type) { o.type = v->type; o.kind = v->kind; changed = true; }
        break;
      }
      case Template::Merge: {
        if (n.outputs[0].type || merge_failed_.count(path_key(n.path))) break;
        std::vector<std::pair<std::string, TypeRef>> ins;
        bool ready = true;
        size_t idx = res_->index.at(path_key(n.path));
        for (const auto& p : n.inputs) {
          bool connected = false;
          for (const auto& e : res_->edges)
            if (e.to == idx && e.to_port == p.name) connected = true;
          if (!connected) continue;
          if (!p.type) { ready = false; break; }
          ins.emplace_back(p.name, p.type);
        }
        if (!ready || ins.empty()) break;
        std::string err;
        if (auto t = types::merged_type(ins, &err)) {
          n.outputs[0].type = *t;
          changed = true;
        } else {
          merge_failed_.insert(path_key(n.path));
          n.diags.push_back(diag(n.path, "invalid_type", "merge type: " + err));
        }
        break;
      }
      default: break;
    }
    return changed;
  }

  void finish_checks() {
    for (size_t i = 0; i < res_->nodes.size(); ++i) {
      Node& n = res_->nodes[i];
      if (n.is_virtual) continue;
      for (const auto& p : n.inputs) {
        bool connected = false;
        for (const auto& e : res_->edges)
          if (e.to == i && e.to_port == p.name) connected = true;
        if (!connected && !p.optional)
          n.exec_diags.push_back(diag(n.path, "input_missing",
                                      "required input " + p.name + " is not connected"));
        else if (connected && !p.type)
          n.diags.push_back(diag(n.path, "unresolved_type",
                                 "input " + p.name + " type is unresolved"));
      }
      if (n.kind == Template::Branch) {
        const auto* c = n.input("condition");
        if (c && c->type && !types::is_condition(*c->type))
          n.diags.push_back(diag(n.path, "invalid_condition",
                                 "branch condition must be a message with value:bool, got " +
                                     c->type->root_type));
      }
      if (n.settings.retain_artifact && n.settings.artifact_output) {
        const auto* o = n.output(*n.settings.artifact_output);
        if (!o)
          n.diags.push_back(diag(n.path, "invalid_setting",
                                 "artifactOutput " + *n.settings.artifact_output + " is not an output"));
        else if (o->kind == PortKind::FlatBuffer)
          n.diags.push_back(diag(n.path, "invalid_setting",
                                 "artifactOutput must be a File or Directory port"));
      }
    }
  }

  // Must-analysis over the flattened graph (greatest fixpoint, so feedback
  // loops converge): a node is guaranteed before n when it lies on every way
  // n can fire. One port: common to all edges into it. all: union over the
  // ports needed to fire. any: common to all connected ports. A branch side
  // therefore never leaks past a merge with the other side.
  void compute_guarantees() {
    size_t n = res_->nodes.size();
    using Set = std::vector<char>;
    std::vector<std::map<std::string, std::vector<size_t>>> in(n);
    for (const auto& e : res_->edges)
      if (e.from != e.to) in[e.to][e.to_port].push_back(e.from);
    // Only nodes that can ever fire take part; others guarantee nothing.
    std::vector<char> reach(n, 0);
    std::vector<size_t> stack;
    for (size_t i = 0; i < n; ++i)
      if (!res_->nodes[i].is_virtual && in[i].empty()) {
        reach[i] = 1;
        stack.push_back(i);
      }
    while (!stack.empty()) {
      size_t i = stack.back();
      stack.pop_back();
      for (const auto& e : res_->edges)
        if (e.from == i && !reach[e.to]) {
          reach[e.to] = 1;
          stack.push_back(e.to);
        }
    }
    std::vector<Set> g(n, Set(n, 1));
    for (size_t i = 0; i < n; ++i)
      if (!reach[i] || in[i].empty()) g[i].assign(n, 0);
    for (bool changed = true; changed;) {
      changed = false;
      for (size_t i = 0; i < n; ++i) {
        if (!reach[i] || in[i].empty()) continue;
        const Node& node = res_->nodes[i];
        std::vector<Set> ports;
        std::vector<bool> required;
        for (const auto& [port, froms] : in[i]) {
          Set s(n, 1);
          bool any_from = false;
          for (size_t u : froms) {
            if (!reach[u]) continue;
            any_from = true;
            Set via = g[u];
            via[u] = 1;
            for (size_t k = 0; k < n; ++k) s[k] &= via[k];
          }
          if (!any_from) continue;
          const auto* p = node.input(port);
          ports.push_back(std::move(s));
          required.push_back(p && !p->optional);
        }
        Set r(n, 0);
        bool any_required = false;
        for (bool b : required) any_required |= b;
        if (node.settings.any_mode || !any_required) {
          r.assign(n, ports.empty() ? 0 : 1);
          for (const auto& s : ports)
            for (size_t k = 0; k < n; ++k) r[k] &= s[k];
        } else {
          for (size_t j = 0; j < ports.size(); ++j)
            if (required[j])
              for (size_t k = 0; k < n; ++k) r[k] |= ports[j][k];
        }
        r[i] = 0;
        if (r != g[i]) {
          g[i] = std::move(r);
          changed = true;
        }
      }
    }
    res_->guaranteed.assign(n, {});
    for (size_t i = 0; i < n; ++i)
      for (size_t k = 0; k < n; ++k)
        if (g[i][k] && !res_->nodes[k].is_virtual) res_->guaranteed[i].push_back(k);
  }

  // Static check of `inputs.upstream."<path>"` / `inputs.local.<id>` in each
  // Code Card's default.nix, so a reference to a Card that is not always
  // upstream is reported before any evaluation. Paths are relative to the
  // project the Card is written in (the same as when that project is opened
  // on its own); `local` names a Card of the same level only. This is early
  // feedback for the written form (nix_references: not in comments or the
  // text of strings); the guarantee is what entry.nix offers, so a reference
  // it cannot see (through an alias, say) fails at evaluation instead.
  void check_nix_references() {
    for (size_t i = 0; i < res_->nodes.size(); ++i) {
      Node& n = res_->nodes[i];
      if (n.kind != Template::Code) continue;
      std::string rel = n.def_dir + "default.nix";
      if (!doc_.sources.count(rel)) continue;
      auto text = read_(rel);
      if (!text) continue;
      auto allowed = res_->nix_upstream(i);
      CardPath scope_path(n.path.begin(), n.path.end() - 1);
      for (const auto& [local, name] : nix_references(*text)) {
        CardPath target = scope_path;
        for (const auto& part : path_from_key(name)) target.push_back(part);
        std::string key = path_key(target);
        auto found = res_->index.find(key);
        if (local && target.size() != n.path.size()) {
          n.diags.push_back(diag(n.path, "nix_reference_unknown",
                                 "default.nix references local." + name + ", but local names a Card of this level only (use upstream.\"" +
                                     name + "\")",
                                 true, rel));
        } else if (target == n.path) {
          n.diags.push_back(diag(n.path, "nix_reference_self", "default.nix references this Card itself (" + key + ")", true, rel));
        } else if (found == res_->index.end()) {
          n.diags.push_back(diag(n.path, "nix_reference_unknown", "default.nix references unknown Card " + key, true, rel));
        } else if (std::find(allowed.begin(), allowed.end(), found->second) == allowed.end()) {
          const Node& t = res_->nodes[found->second];
          std::string why = t.kind != Template::Code ? "is not a code Card"
                            : std::find(res_->guaranteed[i].begin(), res_->guaranteed[i].end(), found->second) ==
                                      res_->guaranteed[i].end()
                                ? "is not upstream on every path to this Card"
                                : "depends on this Card (cycle)";
          n.diags.push_back(diag(n.path, "nix_reference_not_guaranteed", "default.nix references " + key + ", which " + why, true, rel));
        }
      }
    }
  }

  const Document& doc_;
  const SourceReader& read_;
  std::deque<DefinitionDoc> external_defs_;
  std::deque<schema::Project> projects_;
  std::shared_ptr<Resolution> res_;
  std::vector<RawEdge> raw_;
  std::set<std::string> merge_failed_;
};

}  // namespace

std::vector<size_t> Resolution::nix_upstream(size_t node) const {
  std::vector<size_t> out;
  if (node >= guaranteed.size()) return out;
  for (size_t u : guaranteed[node]) {
    if (nodes[u].kind != Template::Code) continue;
    const auto& gu = guaranteed[u];
    if (std::find(gu.begin(), gu.end(), node) != gu.end()) continue;
    out.push_back(u);
  }
  return out;
}

std::shared_ptr<const Resolution> resolve(const Document& doc, const SourceReader& read) {
  return Builder(doc, read).run();
}

}  // namespace tsugumi
