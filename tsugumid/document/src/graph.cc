#include "tsugumi/document/graph.hh"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>

#include "tsugumi/document/collect_type.hh"

namespace tsugumi {

using types::PortKind;
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
    {CardType::Branch, "branch"},     {CardType::Fork, "fork"},
    {CardType::Merge, "merge"},       {CardType::Collect, "collect"},
    {CardType::Subgraph, "subgraph"},
};
}  // namespace

const char* card_type_name(CardType t) {
  for (const auto& [type, name] : kCardTypes)
    if (type == t) return name;
  return "";
}

std::optional<CardType> parse_card_type(std::string_view name) {
  for (const auto& [type, text] : kCardTypes)
    if (name == text) return type;
  return std::nullopt;
}

std::optional<ParamType> parse_param_type(const std::string& s) {
  static const std::map<std::string, ParamType> m = {
      {"bool", ParamType::Bool},
      {"int64", ParamType::Int64},
      {"uint64", ParamType::UInt64},
      {"double", ParamType::Double},
      {"string", ParamType::String},
      {"bool[]", ParamType::BoolArray},
      {"int64[]", ParamType::Int64Array},
      {"uint64[]", ParamType::UInt64Array},
      {"double[]", ParamType::DoubleArray},
      {"string[]", ParamType::StringArray},
  };
  auto it = m.find(s);
  if (it == m.end()) return std::nullopt;
  return it->second;
}

const char* param_type_name(ParamType t) {
  switch (t) {
    case ParamType::Bool:
      return "bool";
    case ParamType::Int64:
      return "int64";
    case ParamType::UInt64:
      return "uint64";
    case ParamType::Double:
      return "double";
    case ParamType::String:
      return "string";
    case ParamType::BoolArray:
      return "bool[]";
    case ParamType::Int64Array:
      return "int64[]";
    case ParamType::UInt64Array:
      return "uint64[]";
    case ParamType::DoubleArray:
      return "double[]";
    case ParamType::StringArray:
      return "string[]";
  }
  return "string";
}

std::string param_fbs_type(ParamType t) {
  switch (t) {
    case ParamType::Bool:
      return "bool";
    case ParamType::Int64:
      return "long";
    case ParamType::UInt64:
      return "ulong";
    case ParamType::Double:
      return "double";
    case ParamType::String:
      return "string";
    case ParamType::BoolArray:
      return "[bool]";
    case ParamType::Int64Array:
      return "[long]";
    case ParamType::UInt64Array:
      return "[ulong]";
    case ParamType::DoubleArray:
      return "[double]";
    case ParamType::StringArray:
      return "[string]";
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

std::optional<std::string> check_param_value(
    ParamType t, const hosq::json::GenericDom<>& v) {
  namespace sv = schema::v;
  bool ok = false;
  switch (t) {
    case ParamType::Bool:
      ok = decodes_as<sv::Bool>(v);
      break;
    case ParamType::Int64:
      ok = decodes_as<sv::Int64>(v);
      break;
    case ParamType::UInt64:
      ok = decodes_as<sv::Uint64>(v);
      break;
    case ParamType::Double:
      ok = decodes_as<schema::Number>(v);
      break;
    case ParamType::String:
      ok = decodes_as<sv::String>(v);
      break;
    case ParamType::BoolArray:
      ok = decodes_as<sv::Array<sv::Bool>>(v);
      break;
    case ParamType::Int64Array:
      ok = decodes_as<sv::Array<sv::Int64>>(v);
      break;
    case ParamType::UInt64Array:
      ok = decodes_as<sv::Array<sv::Uint64>>(v);
      break;
    case ParamType::DoubleArray:
      ok = decodes_as<sv::Array<schema::Number>>(v);
      break;
    case ParamType::StringArray:
      ok = decodes_as<sv::Array<sv::String>>(v);
      break;
  }
  if (ok) return std::nullopt;
  return std::string("expected ") + param_type_name(t) +
         " (integers must be in range, no implicit conversion)";
}

bool has_value_sweep(const Settings& settings) {
  return std::ranges::any_of(
      settings.params, [](const ParamDef& p) { return p.sweep.has_value(); });
}

uint64_t value_sweep_size(const Settings& settings) {
  // Saturates above the limit, so an oversized sweep cannot overflow.
  uint64_t size = 1;
  for (const auto& p : settings.params) {
    if (!p.sweep) continue;
    size = std::min<uint64_t>(
        size * std::min<uint64_t>(p.sweep->size(), kMaxValueSweepSize + 1),
        kMaxValueSweepSize + 1);
  }
  return size;
}

ValueSweepMember value_sweep_member(const Settings& settings, uint64_t index) {
  ValueSweepMember member;
  member.params = settings.params;
  std::vector<std::string> parts;
  for (auto it = member.params.rbegin(); it != member.params.rend(); ++it) {
    if (!it->sweep) continue;
    const auto& values = *it->sweep;
    if (values.empty()) throw std::out_of_range("empty value sweep");
    it->data = values.at(index % values.size());
    index /= values.size();
    // Strings read as they are; other values as their JSON text.
    schema::v::String string;
    const std::string value = schema::decode(*it->data, string)
                                  ? schema::text(*it->data)
                                  : std::string(string.value());
    parts.push_back(it->name + "=" + value);
    it->sweep.reset();
  }
  for (auto it = parts.rbegin(); it != parts.rend(); ++it)
    member.label += (member.label.empty() ? "" : ", ") + *it;
  return member;
}

CardType card_type_of(const DefinitionDoc& def) {
  const auto* name = schema::get(def.card.type);
  const auto type = name ? parse_card_type(*name) : std::nullopt;
  if (!type) throw std::logic_error("card definition without a valid type");
  return *type;
}

std::vector<std::string> fork_outputs(const DefinitionDoc& def) {
  const auto* outputs = schema::get(def.card.outputs);
  if (!outputs) return {"a", "b"};
  std::vector<std::string> names;
  for (const auto& o : *outputs) {
    const auto* name = schema::get(o.name);
    names.push_back(name ? *name : "");
  }
  return names;
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
  Builder(const Document& doc, const SourceReader& read)
      : doc_(doc), read_(read) {}

  std::shared_ptr<Resolution> run() {
    res_ = std::make_shared<Resolution>();
    scope(doc_.project, "", {}, 0);
    connect_all();
    res_->incoming.resize(res_->nodes.size());
    res_->outgoing.resize(res_->nodes.size());
    for (const auto& edge : res_->edges) {
      res_->incoming[edge.to].emplace(edge.to_port, edge.from);
      res_->outgoing[edge.from].push_back(edge.to);
    }
    resolve_types();
    resolve_execution_shapes();
    resolve_boundary_types();
    finish_checks();
    compute_guarantees();
    resolve_nix_inputs();
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

  void timeout(Node& n, const char* key, const hosq::json::GenericDom<>* v,
               std::optional<uint64_t>& out) {
    if (!v) return;
    if (schema::is_null(*v)) {
      out.reset();  // explicit null: no deadline
      return;
    }
    schema::v::Uint64 u;
    if (!schema::decode(*v, u) && u.value() > 0)
      out = u.value();
    else
      n.diags.push_back(
          diag(n.path, "invalid_setting",
               std::string(key) + " must be a positive integer or null"));
  }

  void read_settings(Node& n, const DefinitionDoc* def) {
    auto& s = n.settings;
    s.name = n.path.back();
    if (!def || schema::get(def->card.external)) return;
    const auto& d = def->card;
    auto str = [&](const auto& f, std::string& out) {
      if (const auto* v = schema::get(f)) out = *v;
    };
    auto boolean = [&](const auto& f, bool& out) {
      if (const auto* v = schema::get(f)) out = *v;
    };
    str(d.symbol, s.symbol);
    if (const auto* ins = schema::get(d.inputs);
        ins && n.kind != CardType::Draft) {
      std::set<std::string> seen;
      for (const auto& in : *ins) {
        const auto* name = schema::get(in.name);
        if (!name || !valid_id(*name)) {
          n.diags.push_back(diag(n.path, "invalid_setting",
                                 "inputs entries need a valid name"));
          continue;
        }
        if (!seen.insert(*name).second)
          n.diags.push_back(
              diag(n.path, "invalid_setting", "duplicate input " + *name));
        n.input_defs.push_back({*name});
      }
    }
    if (const auto* v = schema::get(d.max_runs)) {
      if (*v > 0)
        s.max_runs = *v;
      else
        n.diags.push_back(
            diag(n.path, "invalid_setting",
                 "maxRuns must be a positive integer (0 is rejected)"));
    }
    timeout(n, "timeoutSeconds", schema::get(d.timeout_seconds),
            s.timeout_seconds);
    timeout(n, "buildTimeoutSeconds", schema::get(d.build_timeout_seconds),
            s.build_timeout_seconds);
    boolean(d.record_logs, s.record_logs);
    boolean(d.record_io, s.record_io);
    boolean(d.retain_artifact, s.retain_artifact);
    if (const auto* sandbox = schema::get(d.sandbox)) {
      boolean(sandbox->filesystem, s.sandbox.filesystem);
      boolean(sandbox->pid, s.sandbox.pid);
      boolean(sandbox->ipc, s.sandbox.ipc);
      boolean(sandbox->identity, s.sandbox.identity);
      if (const auto* limits = schema::get(sandbox->limits)) {
        if (const auto* v = schema::get(limits->cpu);
            v && !schema::is_null(*v)) {
          schema::Number cpu;
          if (schema::decode(*v, cpu) || !std::isfinite(*cpu) || *cpu <= 0 ||
              *cpu > 256)
            n.diags.push_back(
                diag(n.path, "invalid_setting",
                     "sandbox.limits.cpu must be in (0, 256] or null"));
          else
            s.sandbox.limits.cpu_millis =
                static_cast<uint32_t>(std::ceil(*cpu * 1000));
        }
        if (const auto* v = schema::get(limits->memory_mib);
            v && !schema::is_null(*v)) {
          schema::v::Uint64 mib;
          if (schema::decode(*v, mib) || *mib == 0 ||
              *mib > (uint64_t{1} << 20))
            n.diags.push_back(diag(n.path, "invalid_setting",
                                   "sandbox.limits.memoryMiB must be a "
                                   "positive integer up to 1048576 or null"));
          else
            s.sandbox.limits.memory_bytes = *mib << 20;
        }
      }
    }
    boolean(d.reuse_worker, s.reuse_worker);
    const auto* grants = schema::get(d.grants);
    if (grants) {
      boolean(grants->network, s.grants.network);
      boolean(grants->gui, s.grants.gui);
    }
    if (const auto* v = schema::get(d.execution_service)) {
      if (n.kind != CardType::Code)
        n.diags.push_back(
            diag(n.path, "invalid_setting",
                 "executionService is only available on Code Cards"));
      else if (!valid_id(*v))
        n.diags.push_back(diag(n.path, "invalid_setting",
                               "executionService must be a valid service ID"));
      else
        s.execution_service = *v;
    }
    if (const auto* condition = schema::get(d.worker_condition)) {
      auto compiled = worker_condition::compile(
          {*condition->language, *condition->expression});
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
    if (const auto* secrets = schema::get(d.secrets)) {
      std::set<std::string> names;
      for (const auto& name : *secrets) {
        if (!valid_secret_name(name)) {
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
    if (const auto* v = schema::get(d.system)) {
      if (!remote && *v == native_system())
        s.system.clear();
      else if (*v == "x86_64-linux" || *v == "aarch64-linux")
        s.system = *v;
      else
        n.diags.push_back(diag(
            n.path, "invalid_setting",
            "system must be the native system, x86_64-linux or aarch64-linux"));
    }
    if (const auto* v = schema::get(d.artifact_output)) s.artifact_output = *v;
    const auto* folders = grants ? schema::get(grants->host_paths) : nullptr;
    if (folders) {
      std::set<std::string> names;
      for (const auto& f : *folders) {
        const auto* path = schema::get(f.path);
        const auto* name = schema::get(f.name);
        if (!path || path->empty() || path->front() != '/' || !name ||
            !valid_id(*name)) {
          n.diags.push_back(diag(
              n.path, "invalid_setting",
              "grants.hostPaths entries need an absolute path and a name"));
          continue;
        }
        if (!names.insert(grant_environment(*name)).second) {
          n.diags.push_back(
              diag(n.path, "invalid_setting",
                   "duplicate host path grant variable for " + *name));
          continue;
        }
        const auto* w = schema::get(f.write);
        s.grants.host_paths.push_back({*path, *name, w && *w});
      }
    }
    if (remote && (!s.sandbox.filesystem || !s.sandbox.pid || s.reuse_worker ||
                   s.grants.gui || !s.grants.host_paths.empty()))
      n.diags.push_back(
          diag(n.path, "invalid_setting",
               "external execution requires filesystem and PID isolation "
               "and does not support reuseWorker, GUI or host path grants"));
    read_params(n, def);
  }

  void read_params(Node& n, const DefinitionDoc* def) {
    std::set<std::string> names;
    if (const auto* list = schema::get(def->card.params)) {
      for (const auto& p : *list) {
        const auto* name = schema::get(p.name);
        const auto* type = schema::get(p.type);
        if (!name || !valid_id(*name) || !type) {
          n.diags.push_back(
              diag(n.path, "invalid_param", "params need a name and type"));
          continue;
        }
        auto pt = parse_param_type(*type);
        if (!pt) {
          n.diags.push_back(diag(n.path, "invalid_param",
                                 "Card " + path_key(n.path) + ": param " +
                                     *name + " has unknown type " + *type +
                                     "; write params[].type as bool, int64, "
                                     "uint64, double, string, "
                                     "or one of these with [] for a list"));
          continue;
        }
        if (!names.insert(*name).second) {
          n.diags.push_back(
              diag(n.path, "invalid_param", "duplicate param " + *name));
          continue;
        }
        ParamDef d;
        d.name = *name;
        d.type = *pt;
        if (const auto* nix = schema::get(p.nix)) d.nix = *nix;
        if (const auto* sweep = schema::get(p.sweep)) {
          if (n.kind != CardType::Input) {
            n.diags.push_back(
                diag(n.path, "invalid_param",
                     "param " + d.name +
                         ": sweep is for Input Cards; a Code Card lists its "
                         "implementations in the run list of default.nix"));
          } else if (schema::get(p.data)) {
            n.diags.push_back(
                diag(n.path, "invalid_param",
                     "param " + d.name +
                         ": write data for one value or sweep for one "
                         "invocation per value, not both"));
          } else {
            std::vector<hosq::json::GenericDom<>> values;
            std::optional<std::string> error;
            for (size_t i = 0; i < sweep->size() && !error; ++i) {
              error = check_param_value(d.type, (*sweep)[i]);
              if (error)
                error = "sweep[" + std::to_string(i) + "]: " + *error;
              else
                values.push_back((*sweep)[i]);
            }
            if (error)
              n.diags.push_back(diag(n.path, "invalid_param",
                                     "param " + d.name + ": " + *error));
            else
              d.sweep = std::move(values);
          }
        }
        if (const auto* data = schema::get(p.data)) {
          if (auto err = check_param_value(d.type, *data))
            n.diags.push_back(
                diag(n.path, "invalid_param", "param " + d.name + ": " + *err));
          else
            d.data = *data;
        }
        n.settings.params.push_back(std::move(d));
      }
    }
    for (const auto& p : n.settings.params)
      if (!p.data && !p.sweep)
        n.exec_diags.push_back(
            diag(n.path, "missing_param", "param " + p.name + " has no value"));
    if (const auto size = value_sweep_size(n.settings);
        size > kMaxValueSweepSize)
      n.diags.push_back(
          diag(n.path, "invalid_param",
               "the value sweep makes " + std::to_string(size) +
                   " combinations; at most 256 are allowed per trigger"));
  }

  void load_catalog(Node& n) {
    std::string rel = n.def_dir + "_outputs.fbs";
    if (!doc_.sources.count(rel)) {
      if (n.kind == CardType::Code || n.kind == CardType::Input)
        n.exec_diags.push_back(
            diag(n.path, "missing_outputs", rel + " is missing", true, rel));
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
        n.diags.push_back(diag(n.path, "external_change",
                               rel + " could not be read", true, rel));
        return;
      }
      cat = types::parse_catalog(*text, rel);
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
      d.card = parse_card_json(id, *card);
      d.card_json = *card;
      if (auto meta = read_(n.def_dir + "_meta.json")) {
        check_meta_json(*meta);
        d.meta_json = *meta;
      }
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
  void scope(const schema::Project& project, const std::string& prefix,
             const CardPath& scope_path, int depth) {
    std::string pkgs = project.pkgs_path->value_or("tsugumi:default");
    std::vector<NixSourceRef> sources;
    if (project.nix_sources->has_value())
      for (const auto& s : project.nix_sources->value()) {
        sources.push_back(
            {s.name->value(), s.type->value(), s.ref->value(),
             schema::get(s.overrides)
                 ? schema::canonical_text(*schema::get(s.overrides))
                 : "{}"});
      }
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
      // An unreadable definition is already diagnosed above; it declares no
      // ports and cannot run, like a Draft.
      n.kind = def ? card_type_of(*def) : CardType::Draft;
      // The accepted Document owns this resolution. Edits only affect later
      // runs.
      n.settings.record_logs = project.record_logs->value_or(true);
      n.settings.record_io = project.record_io->value_or(false);
      n.settings.retain_artifact = project.retain_artifact->value_or(false);
      n.settings.log_limit_bytes =
          project.log_limit_bytes->value_or(schema::kDefaultLogLimitBytes);
      n.settings.output_limit_bytes = project.output_limit_bytes->value_or(
          schema::kDefaultOutputLimitBytes);
      n.settings.output_limit_entries = project.output_limit_entries->value_or(
          schema::kDefaultOutputLimitEntries);
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
          // Optional start input: connected, the value is produced when the
          // start signal arrives (e.g. from the trigger); unconnected, the
          // Card is a source that fires once per run.
          n.input_defs = {{"start"}};
          load_catalog(n);
          break;
        case CardType::Trigger:
          n.outputs.push_back(
              {"out", PortKind::FlatBuffer, false, types::empty_type()});
          break;
        case CardType::Branch:
          n.input_defs = {{"value"}, {"condition"}};
          n.outputs = {{"a", PortKind::FlatBuffer, false, nullptr},
                       {"b", PortKind::FlatBuffer, false, nullptr}};
          break;
        case CardType::Fork: {
          // Every output always receives the value, unlike a Branch's.
          n.input_defs = {{"value"}};
          std::set<std::string> seen;
          for (const auto& name : fork_outputs(*def)) {
            if (!valid_id(name))
              n.diags.push_back(diag(n.path, "invalid_setting",
                                     "outputs entries need a valid name"));
            else if (!seen.insert(name).second)
              n.diags.push_back(
                  diag(n.path, "invalid_setting", "duplicate output " + name));
            else
              n.outputs.push_back({name, PortKind::FlatBuffer, false, nullptr});
          }
          if (n.outputs.empty())
            n.diags.push_back(
                diag(n.path, "invalid_setting", "fork needs declared outputs"));
          break;
        }
        case CardType::Merge:
          n.outputs = {{"out", PortKind::FlatBuffer, false, nullptr}};
          if (n.input_defs.empty())
            n.diags.push_back(
                diag(n.path, "invalid_setting", "merge needs declared inputs"));
          break;
        case CardType::Collect:
          n.input_defs = {{"in"}};
          n.outputs = {{"out", PortKind::FlatBuffer, false, nullptr}};
          break;
        case CardType::Draft:
          // Draft wires end on the Card itself. Neither metadata nor a schema
          // declares ports until the Card is implemented.
          n.input_defs.clear();
          n.exec_diags.push_back(
              diag(n.path, "draft",
                   "draft cards are not implemented and cannot run"));
          break;
        case CardType::Subgraph:
          n.is_virtual = true;
          break;
      }
      for (const auto& in : n.input_defs)
        n.inputs.push_back({in.name, PortKind::FlatBuffer, false, nullptr});
      size_t idx = res_->nodes.size();
      res_->index[path_key(n.path)] = idx;
      CardPath inner_scope = n.path;
      const auto* link = def ? schema::get(def->card.external) : nullptr;
      res_->nodes.push_back(std::move(n));

      if (link) {
        // A Card in Card: the project externals/<link>/ of this project.
        std::string ext = prefix + "externals/" + *link + "/";
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
            auto inner = parse_external_project(ext + "tsugumi.json", *text);
            if (const auto* b = schema::get(inner.boundary))
              res_->nodes[idx].boundary = *b;
            projects_.push_back(std::move(inner));
            scope(projects_.back(), ext, inner_scope, depth + 1);
          } catch (const DocumentError& e) {
            res_->nodes[idx].diags.push_back(diag(inner_scope, "invalid_graph",
                                                  e.what(), true,
                                                  ext + "tsugumi.json"));
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
  std::optional<std::pair<size_t, std::string>> leaf(
      CardPath path, std::string port, bool output, const CardPath& report_on,
      const std::string& wire, bool diagnose = true) {
    for (int guard = 0; guard < 32; ++guard) {
      auto it = res_->index.find(path_key(path));
      if (it == res_->index.end()) return std::nullopt;
      Node& n = res_->nodes[it->second];
      if (n.kind != CardType::Subgraph) return std::make_pair(it->second, port);
      const auto* b = n.boundary ? &*n.boundary : nullptr;
      const auto* side =
          b ? (output ? schema::get(b->outputs) : schema::get(b->inputs))
            : nullptr;
      const schema::Endpoint* ep = nullptr;
      if (side)
        for (const auto& [name, e] : side->entries())
          if (name == port) ep = &e;
      if (!ep) {
        if (diagnose)
          n.diags.push_back(diag(n.path, "unknown_port",
                                 wire + ": subgraph has no boundary " +
                                     (output ? "output " : "input ") + port));
        return std::nullopt;
      }
      path.push_back(ep->card->value());
      port = ep->port->value();
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
        if (const auto* ins = schema::get(b->inputs))
          for (const auto& [name, _] : ins->entries())
            n.inputs.push_back({name, PortKind::FlatBuffer, false, nullptr});
        if (const auto* outs = schema::get(b->outputs))
          for (const auto& [name, _] : outs->entries())
            n.outputs.push_back({name, PortKind::FlatBuffer, false, nullptr});
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
                schema::text(schema::string_value(port));
          std::string ports;
          if (output) {
            for (const auto& p : node.outputs) {
              if (!ports.empty()) ports += ", ";
              ports += schema::text(schema::string_value(p.name));
            }
          } else {
            for (const auto& p : node.inputs) {
              if (!ports.empty()) ports += ", ";
              ports += schema::text(schema::string_value(p.name));
            }
          }
          if (!ports.empty())
            fix += "; write " + std::string(side) + ".port as one of " + ports;
          if (!output && node.kind == CardType::Code &&
              (!port.empty() || ports.empty()))
            fix += "; add {\"name\": " +
                   schema::text(
                       schema::string_value(port.empty() ? "source" : port)) +
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
        dn.diags.push_back(diag(dn.path, "multiple_input_wires",
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
  types::PortType* out_port(Node& n, const std::string& name) {
    for (auto& p : n.outputs)
      if (p.name == name) return &p;
    return nullptr;
  }

  void resolve_types() {
    const types::PortType draft{"", PortKind::FlatBuffer, false,
                                types::empty_type()};
    for (size_t pass = 0; pass <= res_->nodes.size() + 1; ++pass) {
      bool changed = false;
      for (const auto& e : res_->edges) {
        if (res_->nodes[e.to].kind == CardType::Draft) continue;
        const auto* src = res_->nodes[e.from].kind == CardType::Draft
                              ? &draft
                              : out_port(res_->nodes[e.from], e.from_port);
        auto* dst = in_port(res_->nodes[e.to], e.to_port);
        if (!src || !src->resolved() || !dst || dst->resolved()) continue;
        dst->type = src->type;
        dst->kind = src->kind;
        dst->merge_member_types = src->merge_member_types;
        changed = true;
      }
      for (auto& n : res_->nodes) changed |= derive(n);
      if (!changed) break;
    }
    for (auto& n : res_->nodes) {
      if (n.is_virtual) continue;
      for (auto& p : n.outputs)
        if (!p.resolved() && n.kind != CardType::Code &&
            n.kind != CardType::Input && connected_out(n, p.name))
          n.diags.push_back(diag(n.path, "unresolved_type",
                                 "output " + p.name +
                                     " type cannot be derived (unresolved "
                                     "cycle or missing input)"));
    }
  }

  bool connected_out(const Node& n, const std::string& port) {
    size_t idx = res_->index.at(path_key(n.path));
    for (const auto& e : res_->edges)
      if (e.from == idx && e.from_port == port) return true;
    return false;
  }

  auto resolve_execution_shapes() -> void {
    using Shape = types::ExecutionShape;
    for (size_t pass = 0; pass <= res_->nodes.size(); ++pass) {
      bool changed = false;
      for (size_t i = 0; i < res_->nodes.size(); ++i) {
        auto& node = res_->nodes[i];
        size_t connected = 0, lists = 0, singles = 0;
        for (auto& port : node.inputs) {
          std::optional<Shape> shape;
          for (const auto& edge : res_->edges) {
            if (edge.to != i || edge.to_port != port.name) continue;
            const auto* output = res_->nodes[edge.from].output(edge.from_port);
            const auto from =
                output ? output->execution_shape : Shape::unspecified;
            shape = !shape || *shape == from ? from : Shape::unspecified;
          }
          if (!shape) continue;
          ++connected;
          if (*shape == Shape::list) ++lists;
          if (*shape == Shape::single) ++singles;
          if (port.execution_shape != *shape) {
            port.execution_shape = *shape;
            changed = true;
          }
        }
        Shape shape = Shape::unspecified;
        if (node.kind == CardType::Collect)
          shape = Shape::single;
        else if (node.kind == CardType::Input && has_value_sweep(node.settings))
          shape = Shape::list;
        else if (lists > 0 &&
                 (node.kind != CardType::Merge || lists == connected))
          shape = Shape::list;
        else if (node.kind != CardType::Code &&
                 node.kind != CardType::Subgraph && singles == connected)
          shape = Shape::single;
        for (auto& port : node.outputs) {
          if (port.execution_shape == shape) continue;
          port.execution_shape = shape;
          changed = true;
        }
      }
      if (!changed) break;
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
          port.kind = resolved->kind;
          port.type = resolved->type;
          port.execution_shape = resolved->execution_shape;
          port.merge_member_types = resolved->merge_member_types;
        }
      };
      inherit(node.inputs, false);
      inherit(node.outputs, true);
    }
  }

  bool derive(Node& n) {
    bool changed = false;
    switch (n.kind) {
      case CardType::Branch:
      case CardType::Fork: {
        const auto* v = n.input("value");
        if (v && v->resolved())
          for (auto& o : n.outputs)
            if (!o.resolved()) {
              o.type = v->type;
              o.kind = v->kind;
              o.merge_member_types = v->merge_member_types;
              changed = true;
            }
        break;
      }
      case CardType::Collect: {
        const auto* in = n.input("in");
        if (!in || !in->resolved() || n.outputs[0].resolved()) break;
        std::string error;
        if (auto type = collect_output_type(*in, error)) {
          n.outputs[0].type = std::move(type);
          n.outputs[0].kind = in->kind;
          changed = true;
        } else if (derive_failed_.insert(path_key(n.path)).second) {
          n.diags.push_back(
              diag(n.path, "invalid_type", "collect type: " + error));
        }
        break;
      }
      case CardType::Merge: {
        auto& out = n.outputs.front();
        if (out.resolved() || derive_failed_.contains(path_key(n.path))) break;
        const size_t idx = res_->index.at(path_key(n.path));
        std::vector<const types::PortType*> ins;
        for (const auto& p : n.inputs)
          if (res_->incoming[idx].contains(p.name)) ins.push_back(&p);
        if (ins.empty() ||
            !std::ranges::all_of(ins, &types::PortType::resolved))
          break;
        std::string err;
        if (auto merged = types::merged_type(ins, &err)) {
          out.type = std::move(merged->type);
          out.kind = ins.front()->kind;
          out.merge_member_types = std::move(merged->member_types);
          n.merge_tags = std::move(merged->tags);
          changed = true;
        } else {
          derive_failed_.insert(path_key(n.path));
          n.diags.push_back(diag(n.path, "invalid_type", "merge type: " + err));
        }
        break;
      }
      default:
        break;
    }
    return changed;
  }

  void finish_checks() {
    for (size_t i = 0; i < res_->nodes.size(); ++i) {
      Node& n = res_->nodes[i];
      if (n.is_virtual) continue;
      for (const auto& p : n.inputs)
        if (res_->incoming[i].contains(p.name) && !p.resolved())
          n.diags.push_back(diag(n.path, "unresolved_type",
                                 "input " + p.name + " type is unresolved"));
      if (n.settings.retain_artifact && n.settings.artifact_output) {
        const auto* o = n.output(*n.settings.artifact_output);
        if (!o)
          n.diags.push_back(diag(n.path, "invalid_setting",
                                 "artifactOutput " +
                                     *n.settings.artifact_output +
                                     " is not an output"));
        else if (o->kind == PortKind::FlatBuffer)
          n.diags.push_back(diag(n.path, "invalid_setting",
                                 "artifactOutput must be a Path port"));
      }
    }
  }

  // Must-analysis over the flattened graph (greatest fixpoint, so feedback
  // loops converge): a node is guaranteed before n when it lies on every way
  // n can fire. Most Cards need every connected input. Merge guarantees
  // only the producers common to all its connected inputs.
  void compute_guarantees() {
    size_t n = res_->nodes.size();
    using Set = std::vector<char>;
    auto in = res_->incoming;
    for (size_t i = 0; i < n; ++i)
      std::erase_if(in[i], [i](const auto& port) { return port.second == i; });
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
      for (const auto to : res_->outgoing[i])
        if (!reach[to]) {
          reach[to] = 1;
          stack.push_back(to);
        }
    }
    std::vector<Set> g(n, Set(n, 1));
    for (size_t i = 0; i < n; ++i)
      if (!reach[i] || in[i].empty()) g[i].assign(n, 0);
    for (bool changed = true; changed;) {
      changed = false;
      for (size_t i = 0; i < n; ++i) {
        if (!reach[i] || in[i].empty()) continue;
        const bool any = res_->nodes[i].kind == CardType::Merge;
        Set r(n, any ? 1 : 0);
        for (const auto& [_, from] : in[i]) {
          if (!reach[from]) continue;
          for (size_t k = 0; k < n; ++k) {
            const bool guaranteed = g[from][k] || k == from;
            if (any)
              r[k] &= guaranteed;
            else
              r[k] |= guaranteed;
          }
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
        if (g[i][k] && !res_->nodes[k].is_virtual)
          res_->guaranteed[i].push_back(k);
  }

  // Follow transparent built-ins to the producers of declared inputs.
  auto trace_nix_producers(size_t source, std::set<size_t>& visiting,
                           std::set<size_t>& result) const -> void {
    const auto kind = res_->nodes[source].kind;
    if (kind != CardType::Collect && kind != CardType::Branch &&
        kind != CardType::Fork && kind != CardType::Merge) {
      result.insert(source);
      return;
    }
    if (!visiting.insert(source).second) {
      result.insert(source);
      return;
    }
    bool found = false;
    for (const auto& edge : res_->edges) {
      if (edge.to != source) continue;
      if (kind == CardType::Branch && edge.to_port != "value") continue;
      found = true;
      trace_nix_producers(edge.from, visiting, result);
    }
    if (!found) result.insert(source);
    visiting.erase(source);
  }

  auto resolve_nix_inputs() -> void {
    for (size_t i = 0; i < res_->nodes.size(); ++i) {
      auto& node = res_->nodes[i];
      if (node.kind != CardType::Code) continue;
      const auto allowed = res_->nix_upstream(i);
      std::map<std::string, std::set<size_t>> producers;
      for (const auto& edge : res_->edges) {
        if (edge.to != i) continue;
        std::set<size_t> visiting;
        trace_nix_producers(edge.from, visiting, producers[edge.to_port]);
      }
      for (const auto& [name, from] : producers) {
        if (name == "tsugumi" || name == "sources") {
          node.diags.push_back(
              diag(node.path, "nix_input_reserved",
                   "input " + name + " is reserved by the Nix SDK"));
          continue;
        }
        if (from.size() != 1) continue;
        const size_t producer = *from.begin();
        if (std::ranges::find(allowed, producer) != allowed.end()) {
          node.nix_inputs.emplace(name, producer);
          node.nix_references.push_back({res_->nodes[producer].path, name});
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
  std::set<std::string> derive_failed_;
};

}  // namespace

std::vector<size_t> Resolution::nix_upstream(size_t node) const {
  std::vector<size_t> out;
  if (node >= guaranteed.size()) return out;
  for (size_t u : guaranteed[node]) {
    if (nodes[u].kind != CardType::Code) continue;
    const auto& gu = guaranteed[u];
    if (std::find(gu.begin(), gu.end(), node) != gu.end()) continue;
    out.push_back(u);
  }
  return out;
}

std::shared_ptr<const Resolution> resolve(const Document& doc,
                                          const SourceReader& read) {
  return Builder(doc, read).run();
}

}  // namespace tsugumi
