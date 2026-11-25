#include "tsugumi/document/graph.hh"

#include <algorithm>
#include <cmath>
#include <deque>
#include <iterator>
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
    {CardType::Trigger, "trigger"},   {CardType::Merge, "merge"},
    {CardType::Collect, "collect"},   {CardType::Subgraph, "subgraph"},
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

auto run_system(const Settings& settings, std::string_view native)
    -> std::string {
  if (settings.systems.empty()) return std::string(native);
  if (std::ranges::find(settings.systems, native) != settings.systems.end())
    return std::string(native);
  return settings.systems.front();
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
      if (node.kind != CardType::Code) continue;
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

  void read_settings(Node& n, const DefinitionDoc* def) {
    auto& s = n.settings;
    s.name = n.path.back();
    if (!def) return;
    const auto& d = def->card();
    if (const auto* inputs = d.inputs())
      for (const auto* input : *inputs)
        n.inputs.push_back({input->name()->str(), false, nullptr});
    if (const auto v = d.maxRuns()) s.max_runs = *v;
    if (const auto v = d.timeoutSeconds(); v && *v) s.timeout_seconds = *v;
    if (const auto v = d.buildTimeoutSeconds(); v && *v)
      s.build_timeout_seconds = *v;

    const auto record = [](D::RecordSetting setting, bool project) {
      return setting == D::RecordSetting::project
                 ? project
                 : setting == D::RecordSetting::on;
    };
    if (const auto v = d.recordLogs())
      s.record_logs = record(*v, s.record_logs);
    if (const auto v = d.recordIO()) s.record_io = record(*v, s.record_io);
    if (const auto* sandbox = d.sandbox()) {
      s.sandbox.filesystem = *sandbox->filesystem();
      s.sandbox.pid = *sandbox->pid();
      s.sandbox.ipc = *sandbox->ipc();
      s.sandbox.identity = *sandbox->identity();
      s.sandbox.limits.cpu_millis =
          static_cast<uint32_t>(std::ceil(*sandbox->limits()->cpu() * 1000));
      s.sandbox.limits.memory_bytes = *sandbox->limits()->memoryMiB() << 20;
    }
    if (const auto v = d.reuseWorker()) s.reuse_worker = *v;
    if (const auto* grants = d.grants()) {
      s.grants.network = *grants->network();
      s.grants.gui = *grants->gui();
      if (const auto* folders = grants->hostPaths())
        for (const auto* folder : *folders)
          s.grants.host_paths.push_back(
              {folder->name()->str(), *folder->write()});
    }
    if (const auto* secrets = d.secrets())
      for (const auto* name : *secrets) s.secrets.push_back(name->str());
    if (const auto* systems = d.systems())
      for (const auto* system : *systems) s.systems.push_back(system->str());
    if (const auto* bits = d.workerCapabilities())
      for (auto bit : *bits) s.worker_capabilities |= uint64_t{1} << bit;
    if (n.kind != CardType::Subgraph) read_params(n);
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
      if (n.kind == CardType::Code)
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
    const auto path = n.def_dir + "_card.json";
    const auto& source = doc_.sources.at(path);
    auto parsed = parse_card_json(n.path.back(), *source.document, path);
    DefinitionDoc d;
    d.card_buffer = std::move(parsed.buffer);
    d.card_json = std::move(parsed.text);
    external_defs_.push_back(std::move(d));
    return &external_defs_.back();
  }

  // One project's canvas: the root project (prefix "") or an external
  // ("externals/<p>/", nested further inside its own externals).
  void scope(const D::Project& project, const std::string& prefix,
             const CardPath& scope_path, int depth) {
    std::vector<NixPathEntry> sources;
    for (const auto* entry : *project.nixPath())
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
      n.settings.record_logs = *project.recordLogs();
      n.settings.record_io = *project.recordIO();
      n.settings.log_limit_bytes = *project.logLimitBytes();
      n.settings.output_limit_bytes = *project.outputLimitBytes();
      n.settings.output_limit_entries = *project.outputLimitEntries();
      read_settings(n, def);
      switch (n.kind) {
        case CardType::Code:
          if (!doc_.sources.count(n.def_dir + "default.nix"))
            n.exec_diags.push_back(diag(n.path, "missing_nix",
                                        "default.nix is missing", true,
                                        n.def_dir + "default.nix"));
          load_catalog(n);
          break;
        case CardType::Trigger:
          n.outputs.push_back(
              {"out", false, types::empty_type()});
          break;
        case CardType::Merge:
          n.outputs = {{"out", false, nullptr}};
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
      const bool subgraph = n.is_virtual;
      res_->nodes.push_back(std::move(n));

      if (subgraph) {
        const auto ext = prefix + "externals/" + id + "/";
        if (depth >= 15) {
          res_->nodes[idx].diags.push_back(
              diag(inner_scope, "invalid_graph",
                   "Card in Card nesting deeper than 15", true, ext));
        } else {
          const auto path = ext + "tsugumi.json";
          const auto& source = doc_.sources.at(path);
          projects_.push_back(
              parse_external_project(path, *source.document).buffer);
          const auto& inner = document_text::root<D::Project>(projects_.back());
          if (const auto* b = inner.boundary())
            res_->nodes[idx].boundary =
                *std::unique_ptr<D::BoundaryT>(b->UnPack());
          scope(inner, ext, inner_scope, depth + 1);
        }
      }
    }
    for (const auto* c : *project.canvas()->connections()) {
      RawEdge e;
      e.scope = scope_path;
      e.from = c->from()->card()->str();
      e.from_port = c->from()->port() ? c->from()->port()->str() : "";
      e.to = c->to()->card()->str();
      e.to_port = c->to()->port() ? c->to()->port()->str() : "";
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

    // What a Card shows to a Card wired from it at the same level:
    // inputs.<name> -> the Cards whose exports are merged under that name.
    using Exposed = std::map<std::string, std::set<CardPath>>;
    std::vector<Exposed> exposed(count);
    // The Cards whose exports a Card stands for when it feeds a subgraph's
    // boundary output.
    std::vector<std::set<CardPath>> own(count);
    const auto same_level = [](const Node& a, const Node& b) {
      return a.project_dir == b.project_dir && a.path.size() == b.path.size() &&
             std::equal(a.path.begin(), a.path.end() - 1, b.path.begin());
    };
    // What one wire from source carries to node. A feedback wire carries
    // nothing. A wire out of a subgraph carries inputs.<subgraph Card id>
    // with the exports of the inner Card; inner ids never appear outside, and
    // Cards inside a subgraph get nothing from outside it.
    const auto carried = [&](size_t index, size_t source) -> Exposed {
      if (component[index] == component[source]) return {};
      const auto& node = res_->nodes[index];
      const auto& producer = res_->nodes[source];
      if (same_level(producer, node)) return exposed[source];
      const auto level = node.path.size() - 1;
      if (producer.path.size() <= node.path.size() ||
          !std::equal(node.path.begin(), node.path.end() - 1,
                      producer.path.begin()) ||
          own[source].empty())
        return {};
      return {{producer.path[level], own[source]}};
    };
    for (auto index : order | std::views::reverse) {
      auto& node = res_->nodes[index];
      std::vector<Exposed> wires;
      std::vector<std::set<CardPath>> owns;
      for (const auto& [_, source] : res_->incoming[index]) {
        wires.push_back(carried(index, source));
        owns.push_back(component[index] == component[source]
                           ? std::set<CardPath>{}
                           : own[source]);
      }
      switch (node.kind) {
        case CardType::Code:
        case CardType::Trigger: {
          Exposed references;
          for (auto& wire : wires)
            for (auto& [name, targets] : wire)
              references[name].insert(targets.begin(), targets.end());
          for (const auto& [name, targets] : references)
            for (const auto& target : targets)
              node.nix_references.push_back({target, name});
          exposed[index] = std::move(references);
          exposed[index][node.path.back()] = {node.path};
          own[index] = {node.path};
          break;
        }
        case CardType::Merge: {
          // Only what every input exposes, under the same name and Cards.
          for (size_t i = 0; i < wires.size(); ++i) {
            if (i == 0) {
              exposed[index] = wires[i];
              own[index] = owns[i];
              continue;
            }
            std::erase_if(exposed[index], [&](const auto& entry) {
              const auto it = wires[i].find(entry.first);
              return it == wires[i].end() || it->second != entry.second;
            });
            std::set<CardPath> common;
            std::ranges::set_intersection(own[index], owns[i],
                                          std::inserter(common, common.end()));
            own[index] = std::move(common);
          }
          break;
        }
        case CardType::Collect:
          // The exports of the Card it collects from.
          for (size_t i = 0; i < wires.size(); ++i) {
            for (auto& [name, targets] : wires[i])
              exposed[index][name].insert(targets.begin(), targets.end());
            own[index].insert(owns[i].begin(), owns[i].end());
          }
          break;
        default:
          // Drafts expose nothing; a subgraph's wires end on its inner Cards.
          break;
      }
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
