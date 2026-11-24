#include "tsugumi/document/graph.hh"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <ranges>
#include <set>
#include <stdexcept>
#include <tuple>

#include "tsugumi/document/collect_type.hh"
#include "tsugumi/document/input_schema.hh"
#include "tsugumi/document/schema_id.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi {

using types::PortType;
using types::TypeRef;

auto Resolution::endpoint(const CardPath& card, const std::string& port,
                          bool output) const
    -> std::optional<std::pair<size_t, std::string>> {
  const auto found = index.find(path_key(card));
  if (found == index.end()) return std::nullopt;
  if (!nodes[found->second].is_virtual) return std::pair{found->second, port};
  const auto alias = endpoints.find({path_key(card), port, output});
  if (alias == endpoints.end()) return std::nullopt;
  return alias->second;
}

namespace {
constexpr std::pair<CardType, const char*> kCardTypes[] = {
    {CardType::Code, "code"},         {CardType::Draft, "draft"},
    {CardType::Trigger, "trigger"},   {CardType::Input, "input"},
    {CardType::Merge, "merge"},       {CardType::Collect, "collect"},
    {CardType::Subgraph, "subgraph"},
};
}  // namespace

const char* card_type_name(CardType t) {
  for (const auto& [type, name] : kCardTypes)
    if (type == t) return name;
  return "";
}

namespace {
auto parse_card_type(std::string_view name) -> std::optional<CardType> {
  for (const auto& [type, text] : kCardTypes)
    if (name == text) return type;
  return std::nullopt;
}

struct ParamTypeInfo {
  ParamType type;
  const char* name;
};
constexpr ParamTypeInfo kParamTypes[] = {
    {ParamType::Bool, "bool"},           {ParamType::Int64, "int"},
    {ParamType::Double, "float"},        {ParamType::String, "string"},
    {ParamType::BoolArray, "[bool]"},    {ParamType::Int64Array, "[int]"},
    {ParamType::DoubleArray, "[float]"}, {ParamType::StringArray, "[string]"},
};

namespace D = Tsugumi::Document;
using document_text::str;

}  // namespace

auto parse_param_type(const std::string& name) -> std::optional<ParamType> {
  for (const auto& info : kParamTypes)
    if (name == info.name) return info.type;
  return std::nullopt;
}

auto param_type_name(ParamType type) -> const char* {
  for (const auto& info : kParamTypes)
    if (type == info.type) return info.name;
  throw std::logic_error("unknown param type");
}

CardType card_type_of(const DefinitionDoc& def) {
  const auto type = def.card().type();
  const auto parsed =
      type ? parse_card_type(D::EnumNameCardType(*type)) : std::nullopt;
  if (!parsed) throw std::logic_error("card definition without a valid type");
  return *parsed;
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

auto Resolution::merge_candidates(size_t node, const std::string& port) const
    -> std::map<std::string, types::TypeRef> {
  if (nodes.at(node).kind != CardType::Merge || !nodes[node].output(port))
    return {};
  std::vector<size_t> pending{node};
  std::set<size_t> visited;
  std::map<std::string, types::TypeRef> candidates;
  while (!pending.empty()) {
    const auto merge = pending.back();
    pending.pop_back();
    if (!visited.insert(merge).second) continue;
    for (const auto& edge : edges) {
      if (edge.to != merge) continue;
      const auto& producer = nodes[edge.from];
      if (producer.kind == CardType::Merge) {
        pending.push_back(edge.from);
        continue;
      }
      auto* output = producer.output(edge.from_port);
      if (output && output->type)
        candidates.try_emplace(schema_id(*output->type), output->type);
    }
  }
  return candidates;
}

const Node* Resolution::find(const CardPath& p) const {
  auto it = index.find(path_key(p));
  return it == index.end() ? nullptr : &nodes[it->second];
}

std::vector<Diag> Resolution::all_diagnostics(bool executable) const {
  std::vector<Diag> out = diags;
  for (const auto& n : nodes) {
    out.insert(out.end(), n.diags.begin(), n.diags.end());
    if (executable)
      out.insert(out.end(), n.exec_diags.begin(), n.exec_diags.end());
  }
  return out;
}

namespace {

Diag diag(const CardPath& card, std::string code, std::string message,
          bool error = true, std::optional<std::string> path = std::nullopt) {
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
  Builder(const Document& doc, const SourceReader& read,
          const ParamsReader& read_params)
      : doc_(doc), read_(read), read_params_(read_params) {}

  std::shared_ptr<Resolution> run() {
    res_ = std::make_shared<Resolution>();
    scope(doc_.project(), "", {}, 0);
    connect_all();
    res_->incoming.resize(res_->nodes.size());
    for (const auto& edge : res_->edges)
      res_->incoming[edge.to].emplace(edge.to_port, edge.from);
    resolve_types();
    resolve_boundary_types();
    resolve_nix_exports();
    for (auto& node : res_->nodes) {
      if (node.kind != CardType::Code && node.kind != CardType::Input) continue;
      try {
        node.generated_inputs = generated_input_schema(*res_, node);
      } catch (const std::runtime_error& error) {
        node.exec_diags.push_back(diag(node.path, "invalid_inputs", error.what(),
                                       true, node.def_dir + "_inputs.fbs"));
      }
    }
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

  void timeout(Node& n, const char* key, std::optional<uint64_t> v,
               std::optional<uint64_t>& out) {
    if (!v) return;
    if (*v > 0)
      out = *v;
    else
      n.diags.push_back(diag(n.path, "invalid_setting",
                             std::string(key) + " must be a positive integer"));
  }

  void read_settings(Node& n, const DefinitionDoc* def) {
    auto& s = n.settings;
    s.name = n.path.back();
    if (!def || def->card().external()) return;
    const auto& d = def->card();
    auto boolean = [&](std::optional<bool> v, bool& out) {
      if (v) out = *v;
    };
    if (const auto* ins = d.inputs(); ins && n.kind != CardType::Draft) {
      std::set<std::string> seen;
      for (const auto* in : *ins) {
        const auto name = in->name()->str();
        if (!valid_id(name)) {
          n.diags.push_back(diag(n.path, "invalid_setting",
                                 "inputs entries need a valid name"));
          continue;
        }
        if (!seen.insert(name).second)
          n.diags.push_back(
              diag(n.path, "invalid_setting", "duplicate input " + name));
        n.inputs.push_back({name, false, nullptr});
      }
    }
    if (const auto v = d.maxRuns()) {
      if (*v > 0)
        s.max_runs = *v;
      else
        n.diags.push_back(
            diag(n.path, "invalid_setting",
                 "maxRuns must be a positive integer (0 is rejected)"));
    }
    timeout(n, "timeoutSeconds", d.timeoutSeconds(), s.timeout_seconds);
    timeout(n, "buildTimeoutSeconds", d.buildTimeoutSeconds(),
            s.build_timeout_seconds);
    boolean(d.recordLogs(), s.record_logs);
    boolean(d.recordIO(), s.record_io);
    if (const auto* sandbox = d.sandbox()) {
      boolean(sandbox->filesystem(), s.sandbox.filesystem);
      boolean(sandbox->pid(), s.sandbox.pid);
      boolean(sandbox->ipc(), s.sandbox.ipc);
      boolean(sandbox->identity(), s.sandbox.identity);
      if (const auto* limits = sandbox->limits()) {
        if (const auto cpu = limits->cpu()) {
          if (!std::isfinite(*cpu) || *cpu <= 0 || *cpu > 256)
            n.diags.push_back(
                diag(n.path, "invalid_setting",
                     "sandbox.limits.cpu must be in (0, 256] or null"));
          else
            s.sandbox.limits.cpu_millis =
                static_cast<uint32_t>(std::ceil(*cpu * 1000));
        }
        if (const auto mib = limits->memoryMiB()) {
          if (*mib == 0 || *mib > (uint64_t{1} << 20))
            n.diags.push_back(diag(n.path, "invalid_setting",
                                   "sandbox.limits.memoryMiB must be a "
                                   "positive integer up to 1048576 or null"));
          else
            s.sandbox.limits.memory_bytes = *mib << 20;
        }
      }
    }
    boolean(d.reuseWorker(), s.reuse_worker);
    const auto* grants = d.grants();
    if (grants) {
      boolean(grants->network(), s.grants.network);
      boolean(grants->gui(), s.grants.gui);
    }
    if (const auto* service = d.executionService()) {
      const auto value = service->str();
      if (n.kind != CardType::Code)
        n.diags.push_back(
            diag(n.path, "invalid_setting",
                 "executionService is only available on Code Cards"));
      else if (!valid_id(value))
        n.diags.push_back(diag(n.path, "invalid_setting",
                               "executionService must be a valid service ID"));
      else
        s.execution_service = value;
    }
    if (const auto* condition = d.workerCondition()) {
      auto compiled = worker_condition::compile(
          {condition->language()->str(), condition->expression()->str()});
      if (const auto* diagnostic =
              std::get_if<worker_condition::Diagnostic>(&compiled))
        n.diags.push_back(diag(n.path, "invalid_setting",
                               "workerCondition: " + diagnostic->message +
                                   " at byte " +
                                   std::to_string(diagnostic->position)));
      else
        s.worker_condition = std::move(
            std::get<std::unique_ptr<const worker_condition::Condition>>(
                compiled));
    }
    const bool remote = s.execution_service != kLocalExecutionService &&
                        s.execution_service != "auto";
    if (const auto* secrets = d.secrets()) {
      std::set<std::string> names;
      for (const auto* entry : *secrets) {
        const auto name = entry->str();
        if (!valid_name(name)) {
          n.diags.push_back(diag(n.path, "invalid_setting",
                                 "secrets entries must match "
                                 "[a-z][a-z0-9_]{0,63}: " +
                                     name));
          continue;
        }
        if (!names.insert(name).second) {
          n.diags.push_back(
              diag(n.path, "invalid_setting", "duplicate secret " + name));
          continue;
        }
        s.secrets.push_back(name);
      }
    }
    if (const auto* system = d.system()) {
      const auto value = system->str();
      if (!remote && value == native_system())
        s.system.clear();
      else if (value == "x86_64-linux" || value == "aarch64-linux")
        s.system = value;
      else
        n.diags.push_back(diag(
            n.path, "invalid_setting",
            "system must be the native system, x86_64-linux or aarch64-linux"));
    }
    const auto* folders = grants ? grants->hostPaths() : nullptr;
    if (folders) {
      std::set<std::string> names;
      for (const auto* f : *folders) {
        const auto path = f->path()->str();
        const auto name = f->name()->str();
        if (path.empty() || path.front() != '/' || !valid_id(name)) {
          n.diags.push_back(diag(
              n.path, "invalid_setting",
              "grants.hostPaths entries need an absolute path and a name"));
          continue;
        }
        if (!names.insert(grant_environment(name)).second) {
          n.diags.push_back(
              diag(n.path, "invalid_setting",
                   "duplicate host path grant variable for " + name));
          continue;
        }
        s.grants.host_paths.push_back({path, name, f->write().value_or(false)});
      }
    }
    if (remote && (!s.sandbox.filesystem || !s.sandbox.pid || s.reuse_worker ||
                   s.grants.gui || !s.grants.host_paths.empty()))
      n.diags.push_back(
          diag(n.path, "invalid_setting",
               "external execution requires filesystem and PID isolation "
               "and does not support reuseWorker, GUI or host path grants"));
    read_params(n);
  }

  void read_params(Node& n) {
    const auto rel = n.def_dir + "params.nix";
    const auto invalid = [&](const std::string& message) {
      n.diags.push_back(diag(n.path, "invalid_param", message, true, rel));
    };
    const auto source = doc_.sources.find(rel);
    if (source == doc_.sources.end() || !read_params_) return;
    try {
      if (source->second.size > kMaxDocumentJsonBytes)
        throw std::runtime_error(rel + ": exceeds the 8 MiB document limit");
      auto bytes = read_(rel);
      if (!bytes)
        throw std::runtime_error(rel +
                                 " differs from the confirmed document");
      for (const auto& raw : json::Array(read_params_(n.def_dir, *bytes))) {
        const auto entry = json::Object(raw);
        const auto name = json::Text(entry, "name");
        const auto type = parse_param_type(json::Text(entry, "type"));
        if (!type || !valid_id(name)) {
          invalid("invalid value for param " + name);
          continue;
        }
        n.settings.params.push_back({name, *type, entry.at("value")});
      }
    } catch (const std::exception& error) {
      invalid(error.what());
    }
  }

  void load_catalog(Node& n) {
    std::string rel = n.def_dir + "_outputs.fbs";
    if (!doc_.sources.count(rel)) {
      if (n.kind == CardType::Code || n.kind == CardType::Input)
        n.exec_diags.push_back(
            diag(n.path, "missing_outputs", rel + " is missing", true, rel));
      return;
    }
    // Included definition schemas participate in the same cache key.
    std::string key = rel;
    for (auto it = doc_.sources.lower_bound(n.def_dir);
         it != doc_.sources.end() && it->first.starts_with(n.def_dir); ++it)
      if (it->first.ends_with(".fbs") && it->first != n.def_dir + "_inputs.fbs")
        key += '\0' + it->first + '\0' + it->second.hash;
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
        n.diags.push_back(diag(n.path, "external_change",
                               rel + " could not be read", true, rel));
        return;
      }
      cat = types::parse_catalog(*text, rel, [&](const std::string& include) {
        // Output types must be available independently of incoming wiring.
        return include == n.def_dir + "_inputs.fbs" ? std::nullopt : read_(include);
      });
      std::lock_guard<std::mutex> l(mu);
      if (parsed.size() >= 2048) parsed.clear();
      parsed[key] = *cat;
    }
    for (const auto& e : cat->errors)
      n.diags.push_back(diag(n.path, "invalid_outputs", e, true, rel));
    n.outputs = cat->ports;
  }

  // The definition of a Card written in an external project (read as sources).
  const DefinitionDoc* external_definition(Node& n) {
    auto card = read_(n.def_dir + "_card.json");
    if (!card) {
      n.diags.push_back(
          diag(n.path, "invalid_document", n.def_dir + " needs _card.json"));
      return nullptr;
    }
    DefinitionDoc d;
    const std::string& id = n.path.back();
    try {
      auto parsed = parse_card_json(id, *card);
      d.card_buffer = std::move(parsed.buffer);
      d.card_json = std::move(parsed.text);
    } catch (const DocumentError& e) {
      n.diags.push_back(
          diag(n.path, "invalid_document", e.what(), true, n.def_dir));
      return nullptr;
    }
    external_defs_.push_back(std::move(d));
    return &external_defs_.back();
  }

  // One project's canvas: the root project (prefix "") or an external
  // ("externals/<p>/", nested further inside its own externals).
  void scope(const D::Project& project, const std::string& prefix,
             const CardPath& scope_path, int depth) {
    std::vector<NixPathEntry> sources;
    if (const auto* list = project.nixPath())
      for (const auto* entry : *list)
        sources.push_back({entry->name()->str(), entry->url()->str()});

    for (const auto* card : *project.canvas()->cards()) {
      const auto id = card->id()->str();
      Node n;
      n.path = scope_path;
      n.path.push_back(id);
      n.def_dir = prefix + "definitions/" + id + "/";
      n.project_dir = prefix;
      n.nix_path = sources;
      const DefinitionDoc* def = nullptr;
      if (prefix.empty()) {
        auto it = doc_.definitions.find(id);
        if (it != doc_.definitions.end()) def = &it->second;
      } else {
        def = external_definition(n);
      }
      // An unreadable definition is already diagnosed above; it declares no
      // ports and cannot run, like a Draft.
      n.kind = def ? card_type_of(*def) : CardType::Draft;
      // The accepted Document owns this resolution. Edits only affect later
      // runs.
      n.settings.record_logs = project.recordLogs().value_or(true);
      n.settings.record_io = project.recordIO().value_or(false);
      n.settings.log_limit_bytes =
          project.logLimitBytes().value_or(kDefaultLogLimitBytes);
      n.settings.output_limit_bytes =
          project.outputLimitBytes().value_or(kDefaultOutputLimitBytes);
      n.settings.output_limit_entries =
          project.outputLimitEntries().value_or(kDefaultOutputLimitEntries);
      read_settings(n, def);
      switch (n.kind) {
        case CardType::Code:
          if (!doc_.sources.count(n.def_dir + "default.nix"))
            n.exec_diags.push_back(diag(n.path, "missing_nix",
                                        "default.nix is missing", true,
                                        n.def_dir + "default.nix"));
          load_catalog(n);
          break;
        case CardType::Input:
          if (!doc_.sources.count(n.def_dir + "default.nix"))
            n.exec_diags.push_back(diag(n.path, "missing_nix",
                                        "default.nix is missing", true,
                                        n.def_dir + "default.nix"));
          // A connected start input triggers the Card on arrival.
          // An unconnected Input Card is a source that fires once per run.
          n.inputs = {{"start", false, nullptr}};
          load_catalog(n);
          break;
        case CardType::Trigger:
          n.outputs.push_back(
              {"out", false, types::empty_type()});
          break;
        case CardType::Merge:
          n.outputs = {{"out", false, nullptr}};
          if (n.inputs.empty())
            n.diags.push_back(
                diag(n.path, "invalid_setting", "merge needs declared inputs"));
          break;
        case CardType::Collect:
          n.inputs = {{"in", false, nullptr}};
          n.outputs = {{"out", false, nullptr}};
          break;
        case CardType::Draft:
          // Draft wires end on the Card itself. Neither metadata nor a schema
          // declares ports until the Card is implemented.
          n.inputs.clear();
          n.exec_diags.push_back(
              diag(n.path, "draft",
                   "draft cards are not implemented and cannot run"));
          break;
        case CardType::Subgraph:
          n.is_virtual = true;
          break;
      }
      size_t idx = res_->nodes.size();
      res_->index[path_key(n.path)] = idx;
      CardPath inner_scope = n.path;
      const auto* link = def ? def->card().external() : nullptr;
      res_->nodes.push_back(std::move(n));

      if (link) {
        // A Card in Card: the project externals/<link>/ of this project.
        std::string ext = prefix + "externals/" + link->str() + "/";
        res_->nodes[idx].external = ext;
        auto text = read_(ext + "tsugumi.json");
        if (!text) {
          res_->nodes[idx].diags.push_back(diag(
              inner_scope, "unknown_external",
              ext + " is not a Tsugumi project (no tsugumi.json)", true, ext));
        } else if (depth >= 15) {
          res_->nodes[idx].diags.push_back(
              diag(inner_scope, "invalid_graph",
                   "Card in Card nesting deeper than 15", true, ext));
        } else {
          try {
            projects_.push_back(
                parse_external_project(ext + "tsugumi.json", *text).buffer);
            const auto& inner =
                document_text::root<D::Project>(projects_.back());
            if (const auto* b = inner.boundary())
              res_->nodes[idx].boundary =
                  *std::unique_ptr<D::BoundaryT>(b->UnPack());
            scope(inner, ext, inner_scope, depth + 1);
          } catch (const DocumentError& e) {
            res_->nodes[idx].diags.push_back(diag(inner_scope, "invalid_graph",
                                                  e.what(), true,
                                                  ext + "tsugumi.json"));
          }
        }
      }
    }
    for (const auto* c : *project.canvas()->connections()) {
      RawEdge e;
      e.scope = scope_path;
      e.from = c->from()->card()->str();
      e.from_port = c->from()->port()->str();
      e.to = c->to()->card()->str();
      e.to_port = c->to()->port()->str();
      raw_.push_back(std::move(e));
    }
  }

  // Resolves an endpoint through subgraph boundaries down to a leaf node.
  std::optional<std::pair<size_t, std::string>> leaf(
      CardPath path, std::string port, bool output, const CardPath& report_on,
      const std::string& wire, bool diagnose = true) {
    for (int guard = 0; guard < 32; ++guard) {
      auto it = res_->index.find(path_key(path));
      if (it == res_->index.end()) return std::nullopt;
      Node& n = res_->nodes[it->second];
      if (n.kind != CardType::Subgraph) return std::make_pair(it->second, port);
      const auto* b = n.boundary ? &*n.boundary : nullptr;
      const auto* side = b ? (output ? &b->outputs : &b->inputs) : nullptr;
      const D::BoundaryPortT* ep = nullptr;
      if (side)
        for (const auto& e : *side)
          if (e->name == port) ep = e.get();
      if (!ep) {
        if (diagnose)
          n.diags.push_back(diag(n.path, "unknown_port",
                                 wire + ": subgraph has no boundary " +
                                     (output ? "output " : "input ") + port));
        return std::nullopt;
      }
      path.push_back(ep->card);
      port = ep->port;
      if (!res_->index.count(path_key(path))) {
        if (diagnose)
          n.diags.push_back(diag(n.path, "unknown_card",
                                 wire + ": boundary " + port +
                                     " points to unknown card " +
                                     path_key(path)));
        return std::nullopt;
      }
    }
    if (diagnose)
      res_->diags.push_back(diag(report_on, "invalid_graph",
                                 "subgraph boundary nesting too deep"));
    return std::nullopt;
  }

  void connect_all() {
    // Boundary ports become ports of the virtual subgraph node for display.
    for (auto& n : res_->nodes) {
      if (n.kind != CardType::Subgraph) continue;
      if (const auto* b = n.boundary ? &*n.boundary : nullptr) {
        for (const auto& port : b->inputs)
          n.inputs.push_back({port->name, false, nullptr});
        for (const auto& port : b->outputs)
          n.outputs.push_back(
              {port->name, false, nullptr});
      }
    }
    for (const auto& node : res_->nodes) {
      if (!node.is_virtual) continue;
      for (const bool output : {false, true})
        for (const auto& port : output ? node.outputs : node.inputs)
          if (auto end = leaf(node.path, port.name, output, node.path,
                              "boundary", false))
            res_->endpoints[{path_key(node.path), port.name, output}] = *end;
    }
    std::set<std::pair<size_t, std::string>> wired_inputs;
    for (const auto& e : raw_) {
      CardPath from = e.scope;
      CardPath to = e.scope;
      from.push_back(e.from);
      to.push_back(e.to);
      const auto wire = "wire " + path_key(from) + " -> " + path_key(to);
      auto src = leaf(from, e.from_port, true, from, wire);
      auto dst = leaf(to, e.to_port, false, to, wire);
      if (!src || !dst) continue;
      Node& sn = res_->nodes[src->first];
      Node& dn = res_->nodes[dst->first];
      bool ok = true;
      const auto check = [&](Node& node, const std::string& port, bool output) {
        const auto side = output ? "from" : "to";
        const auto card = path_key(node.path);
        const bool draft = node.kind == CardType::Draft;
        if (draft ? port.empty()
                  : (output ? node.output(port) != nullptr
                            : node.input(port) != nullptr))
          return;
        std::string fix;
        if (draft) {
          fix = card + " is still a Draft, so its " + side +
                " end keeps port \"\"";
        } else {
          const auto direction = output ? "output" : "input";
          fix = card + " has no " + direction + " port " +
                json::Quote(port);
          std::string ports;
          if (output) {
            for (const auto& p : node.outputs) {
              if (!ports.empty()) ports += ", ";
              ports += json::Quote(p.name);
            }
          } else {
            for (const auto& p : node.inputs) {
              if (!ports.empty()) ports += ", ";
              ports += json::Quote(p.name);
            }
          }
          if (!ports.empty())
            fix += "; write " + std::string(side) + ".port as one of " + ports;
          if (!output && node.kind == CardType::Code &&
              (!port.empty() || ports.empty()))
            fix += "; add {\"name\": " +
                   json::Quote(port.empty() ? "source" : port) +
                   "} to " + node.def_dir +
                   "_card.json inputs and write to.port with that name";
          else if (ports.empty())
            fix += "; declare an " + std::string(direction) + " port on " +
                   card + " and write " + side + ".port with its name";
        }
        node.diags.push_back(
            diag(node.path, "unknown_port", wire + ": " + fix));
        ok = false;
      };
      check(sn, src->second, true);
      check(dn, dst->second, false);
      if (!ok) continue;
      if (dn.kind != CardType::Draft &&
          !wired_inputs.emplace(dst->first, dst->second).second) {
        dn.diags.push_back(diag(dn.path, "input_already_connected",
                                wire + ": input " + dst->second +
                                    " already has a wire; use a Merge Card "
                                    "to combine arrivals"));
        continue;
      }
      res_->edges.push_back({src->first, src->second, dst->first, dst->second});
    }
  }

  types::PortType* in_port(Node& n, const std::string& name) {
    for (auto& p : n.inputs)
      if (p.name == name) return &p;
    return nullptr;
  }
  void resolve_types() {
    const types::PortType draft{.name = "", .type = types::empty_type()};
    for (auto& node : res_->nodes) {
      if (node.kind != CardType::Collect) continue;
      std::string error;
      node.outputs.front().type = collect_output_type(error);
      if (!node.outputs.front().type)
        node.diags.push_back(
            diag(node.path, "invalid_type", "collect type: " + error));
    }
    for (size_t i = 0; i < res_->nodes.size(); ++i) {
      auto& node = res_->nodes[i];
      if (node.kind != CardType::Merge) continue;
      auto& output = node.outputs.front();
      const auto candidates = res_->merge_candidates(i, output.name);
      if (candidates.size() == 1) output.type = candidates.begin()->second;
      const auto short_name = [](const std::string& full) {
        return full.substr(full.find_last_of('.') + 1);
      };
      std::map<std::string, size_t> name_counts;
      for (const auto& [_, type] : candidates)
        ++name_counts[short_name(type->root_type)];
      for (const auto& [_, type] : candidates) {
        auto name = short_name(type->root_type);
        output.merge_member_types.push_back(
            name_counts.at(name) == 1 ? std::move(name) : type->root_type);
      }
      std::ranges::sort(output.merge_member_types);
    }
    for (const auto& edge : res_->edges) {
      auto& target = res_->nodes[edge.to];
      const auto& source = res_->nodes[edge.from];
      const auto* from = source.kind == CardType::Draft
                             ? &draft
                             : source.output(edge.from_port);
      auto* to = in_port(target, edge.to_port);
      if (!from || !to) continue;
      to->type = from->type;
      to->merge_member_types = from->merge_member_types;
    }
  }

  auto resolve_boundary_types() -> void {
    for (auto& node : res_->nodes) {
      if (!node.is_virtual) continue;
      auto inherit = [&](auto& ports, bool output) {
        for (auto& port : ports) {
          const auto end = res_->endpoint(node.path, port.name, output);
          if (!end) continue;
          const auto& leaf = res_->nodes[end->first];
          const auto* resolved =
              output ? leaf.output(end->second) : leaf.input(end->second);
          if (!resolved) continue;
          port.type = resolved->type;
          port.merge_member_types = resolved->merge_member_types;
        }
      };
      inherit(node.inputs, false);
      inherit(node.outputs, true);
    }
  }

  auto resolve_nix_exports() -> void {
    const size_t count = res_->nodes.size();
    std::vector<std::vector<size_t>> outgoing(count);
    for (const auto& edge : res_->edges)
      outgoing[edge.from].push_back(edge.to);
    std::vector<bool> visited(count);
    std::vector<size_t> order;
    const auto finish = [&](auto&& self, size_t node) -> void {
      if (visited[node]) return;
      visited[node] = true;
      for (auto next : outgoing[node]) self(self, next);
      order.push_back(node);
    };
    for (size_t node = 0; node < count; ++node) finish(finish, node);

    // Edges within a strongly connected component are feedback edges.
    std::vector<size_t> component(count, count);
    const auto assign = [&](auto&& self, size_t node, size_t group) -> void {
      if (component[node] != count) return;
      component[node] = group;
      for (const auto& [_, source] : res_->incoming[node])
        self(self, source, group);
    };
    for (auto node : order | std::views::reverse) assign(assign, node, node);

    const auto normal = [](CardType kind) {
      // TODO(open): exports across Merge, Collect, and Draft Cards.
      return kind == CardType::Code || kind == CardType::Input ||
             kind == CardType::Trigger;
    };
    for (auto index : order | std::views::reverse) {
      auto& node = res_->nodes[index];
      if (!normal(node.kind)) continue;
      std::map<std::string, CardPath> exports;
      for (const auto& [_, source] : res_->incoming[index]) {
        const auto& producer = res_->nodes[source];
        if (!normal(producer.kind)) continue;
        // TODO(open): exports across feedback wires.
        if (component[index] == component[source]) continue;
        // TODO(open): exports across nested and external Card boundaries.
        if (producer.project_dir != node.project_dir ||
            producer.path.size() != node.path.size() ||
            !std::equal(producer.path.begin(), producer.path.end() - 1,
                        node.path.begin()))
          continue;
        exports.emplace(producer.path.back(), producer.path);
        for (const auto& ref : producer.nix_references)
          exports.emplace(ref.name, ref.target);
      }
      for (auto& [id, target] : exports)
        node.nix_references.push_back({std::move(target), id});
    }
  }

  const Document& doc_;
  const SourceReader& read_;
  const ParamsReader& read_params_;
  std::deque<DefinitionDoc> external_defs_;
  std::deque<std::string> projects_;  // nested tsugumi.json buffers
  std::shared_ptr<Resolution> res_;
  std::vector<RawEdge> raw_;
};

}  // namespace

std::shared_ptr<const Resolution> resolve(const Document& doc,
                                          const SourceReader& read,
                                          const ParamsReader& read_params) {
  return Builder(doc, read, read_params).run();
}

}  // namespace tsugumi
