#include "graph.hpp"

#include <limits>
#include <regex>
#include <set>

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
    scope(doc_.project.canvas->value(), {}, 0);
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
    std::string from, from_port, to, to_port;
  };

  Template template_of(const schema::CardInstance& card, const DefinitionDoc* def) {
    if (card.canvas->has_value() || card.project->has_value()) return Template::Subgraph;
    if (!def) return Template::Draft;
    const auto* t = schema::get(def->card.template_name);
    if (!t || *t == "none" || t->empty()) return Template::Code;
    static const std::map<std::string, Template> m = {
        {"trigger", Template::Trigger}, {"input", Template::Input},   {"fork", Template::Fork},
        {"branch", Template::Branch},   {"merge", Template::Merge},   {"logger", Template::Logger},
        {"subgraph", Template::Subgraph}, {"draft", Template::Draft},
    };
    auto it = m.find(*t);
    return it == m.end() ? Template::Code : it->second;
  }

  // Instance override, else definition default, else nullptr (builtin default).
  template <class F>
  static auto pick(const F& inst, const auto* def_field) -> decltype(schema::get(inst)) {
    if (const auto* v = schema::get(inst)) return v;
    return def_field ? schema::get(*def_field) : nullptr;
  }

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
    const auto& inst = n.instance;
    const schema::CardDefinition* d = def ? &def->card : nullptr;
    auto str = [&](const auto& i, const auto* df, std::string& out) {
      if (const auto* v = pick(i, df)) out = *v;
    };
    auto boolean = [&](const auto& i, const auto* df, bool& out) {
      if (const auto* v = pick(i, df)) out = *v;
    };
    str(inst.name, d ? &d->name : nullptr, s.name);
    str(inst.symbol, d ? &d->symbol : nullptr, s.symbol);
    str(inst.description, d ? &d->description : nullptr, s.description);
    if (d) {
      if (const auto* lang = schema::get(d->language)) s.language = *lang == "none" ? "" : *lang;
      if (const auto* mode = schema::get(d->input_mode)) {
        if (*mode == "all" || *mode == "any") s.any_mode = *mode == "any";
        else n.diags.push_back(diag(n.path, "invalid_setting", "inputMode must be all or any"));
      }
      if (const auto* ins = schema::get(d->inputs)) {
        std::set<std::string> seen;
        for (const auto& in : *ins) {
          const auto* name = schema::get(in.name);
          if (!name || !valid_id(*name)) {
            n.diags.push_back(diag(n.path, "invalid_setting", "inputs entries need a valid name"));
            continue;
          }
          if (!seen.insert(*name).second)
            n.diags.push_back(diag(n.path, "invalid_setting", "duplicate input " + *name));
          const auto* opt = schema::get(in.optional);
          n.input_defs.push_back({*name, opt && *opt});
        }
      }
    }
    if (const auto* v = pick(inst.max_runs, d ? &d->max_runs : nullptr)) {
      if (*v > 0) s.max_runs = *v;
      else n.diags.push_back(diag(n.path, "invalid_setting", "maxRuns must be a positive integer (0 is rejected)"));
    }
    timeout(n, "timeoutSeconds", pick(inst.timeout_seconds, d ? &d->timeout_seconds : nullptr), s.timeout_seconds);
    timeout(n, "buildTimeoutSeconds", pick(inst.build_timeout_seconds, d ? &d->build_timeout_seconds : nullptr),
            s.build_timeout_seconds);
    boolean(inst.record_io, d ? &d->record_io : nullptr, s.record_io);
    boolean(inst.retain_artifact, d ? &d->retain_artifact : nullptr, s.retain_artifact);
    boolean(inst.filesystem, d ? &d->filesystem : nullptr, s.filesystem);
    boolean(inst.sandbox, d ? &d->sandbox : nullptr, s.sandbox);
    boolean(inst.canvas_enabled, d ? &d->canvas_enabled : nullptr, s.canvas_enabled);
    if (const auto* v = pick(inst.artifact_output, d ? &d->artifact_output : nullptr)) s.artifact_output = *v;
    if (const auto* v = pick(inst.logger_database, d ? &d->logger_database : nullptr)) {
      s.logger_database = *v;
      bool found = false;
      for (const auto& db : doc_.database_connections()) found |= db.name->value() == *v;
      if (!found)
        n.diags.push_back(diag(n.path, "unknown_database", "database connection " + *v + " is not defined"));
    }
    read_params(n, def);
  }

  void read_params(Node& n, const DefinitionDoc* def) {
    std::set<std::string> names;
    if (def) {
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
              n.diags.push_back(diag(n.path, "invalid_param", "param " + d.name + " default: " + *err));
            else d.data = *data;
          }
          n.settings.params.push_back(std::move(d));
        }
      }
    }
    if (const auto* overrides = schema::get(n.instance.params)) {
      for (const auto& [key, value] : overrides->entries()) {
        auto it = std::find_if(n.settings.params.begin(), n.settings.params.end(),
                               [&](const ParamDef& d) { return d.name == key; });
        if (it == n.settings.params.end()) {
          n.diags.push_back(diag(n.path, "unknown_param", "param " + key + " is not defined"));
          continue;
        }
        if (auto err = check_param_value(it->type, value))
          n.diags.push_back(diag(n.path, "invalid_param", "param " + key + ": " + *err));
        else it->data = value;
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
    std::string rel = "definitions/" + n.definition + "/_outputs.fbs";
    if (!doc_.sources.count(rel)) {
      if (n.kind == Template::Code || n.kind == Template::Input)
        n.exec_diags.push_back(diag(n.path, "missing_outputs", rel + " is missing", true, rel));
      return;
    }
    auto text = read_(rel);
    if (!text) {
      n.diags.push_back(diag(n.path, "external_change", rel + " could not be read", true, rel));
      return;
    }
    auto cat = types::parse_catalog(*text, rel);
    for (const auto& e : cat.errors) n.diags.push_back(diag(n.path, "invalid_outputs", e, true, rel));
    n.outputs = std::move(cat.ports);
  }

  void scope(const schema::Graph& graph, const CardPath& scope_path, int depth) {
    for (const auto& card : graph.cards->value()) {
      Node n;
      n.path = scope_path;
      n.path.push_back(card.id->value());
      n.instance = card;
      const DefinitionDoc* def = nullptr;
      if (const auto* d = schema::get(card.definition)) {
        n.definition = *d;
        def = &doc_.definitions.at(n.definition);
      }
      n.kind = template_of(card, def);
      read_settings(n, def);
      switch (n.kind) {
        case Template::Code:
          if (n.settings.language != "cpp" && n.settings.language != "python" &&
              n.settings.language != "bash")
            n.exec_diags.push_back(diag(n.path, "invalid_language",
                                        "language must be cpp, python or bash"));
          if (!doc_.sources.count("definitions/" + n.definition + "/default.nix"))
            n.exec_diags.push_back(diag(n.path, "missing_nix", "default.nix is missing", true,
                                        "definitions/" + n.definition + "/default.nix"));
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
          // Provisional outputs let a Draft be wired before it is implemented:
          // typed ones from _outputs.fbs, otherwise the names (and purposes)
          // in _meta.json, carrying an empty message until implemented.
          load_catalog(n);
          if (n.outputs.empty() && def && def->meta)
            if (const auto* outs = schema::get(def->meta->outputs))
              for (const auto& o : *outs)
                if (const auto* name = schema::get(o.name); name && valid_id(*name))
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
      res_->nodes.push_back(std::move(n));

      if (card.canvas->has_value()) {
        scope(inner_graph(card), inner_scope, depth + 1);
      } else if (const auto* rel = schema::get(card.project)) {
        auto text = read_(*rel);
        if (!text) {
          res_->nodes[idx].diags.push_back(diag(inner_scope, "external_change", *rel + " could not be read"));
        } else if (depth > 15) {
          res_->nodes[idx].diags.push_back(diag(inner_scope, "invalid_graph", "graph nesting too deep"));
        } else {
          try {
            scope(parse_graph_file(*rel, *text), inner_scope, depth + 1);
          } catch (const DocumentError& e) {
            res_->nodes[idx].diags.push_back(diag(inner_scope, "invalid_graph", e.what(), true, *rel));
          }
        }
      }
    }
    for (const auto& c : graph.connections->value()) {
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
      const auto* b = schema::get(n.instance.boundary);
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
      if (const auto* b = schema::get(n.instance.boundary)) {
        if (const auto* ins = schema::get(b->inputs))
          for (const auto& [name, _] : ins->entries()) n.inputs.push_back({name, PortKind::FlatBuffer, true, nullptr});
        if (const auto* outs = schema::get(b->outputs))
          for (const auto& [name, _] : outs->entries()) n.outputs.push_back({name, PortKind::FlatBuffer, true, nullptr});
      }
    }
    for (const auto& e : raw_) {
      CardPath from = e.scope, to = e.scope;
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
  // upstream is reported before any evaluation.
  void check_nix_references() {
    static const std::regex re(R"re(\b(upstream|local)\s*\.\s*(?:"([^"]+)"|([A-Za-z_][A-Za-z0-9_'-]*)))re");
    for (size_t i = 0; i < res_->nodes.size(); ++i) {
      Node& n = res_->nodes[i];
      if (n.kind != Template::Code) continue;
      std::string rel = "definitions/" + n.definition + "/default.nix";
      if (!doc_.sources.count(rel)) continue;
      auto text = read_(rel);
      if (!text) continue;
      auto allowed = res_->nix_upstream(i);
      CardPath scope_path(n.path.begin(), n.path.end() - 1);
      for (auto it = std::sregex_iterator(text->begin(), text->end(), re); it != std::sregex_iterator(); ++it) {
        const auto& m = *it;
        std::string name = m[2].matched ? m[2].str() : m[3].str();
        CardPath target;
        if (m[1] == "local") {
          target = scope_path;
          target.push_back(name);
        } else {
          target = path_from_key(name);
        }
        std::string key = path_key(target);
        auto found = res_->index.find(key);
        if (target == n.path) {
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
