#include "tsugumi/document/graph.hh"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>

#include "tsugumi/document/collect_type.hh"
#include "tsugumi/document/schema_id.hh"
#include "tsugumi/util/json_object.hh"

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

namespace {
struct ParamTypeInfo {
  ParamType type;
  const char* name;
  const char* fbs;
};
constexpr ParamTypeInfo kParamTypes[] = {
    {ParamType::Bool, "bool", "bool"},
    {ParamType::Int64, "int", "long"},
    {ParamType::Double, "float", "double"},
    {ParamType::String, "string", "string"},
    {ParamType::BoolArray, "[bool]", "[bool]"},
    {ParamType::Int64Array, "[int]", "[long]"},
    {ParamType::DoubleArray, "[float]", "[double]"},
    {ParamType::StringArray, "[string]", "[string]"},
};

namespace D = Tsugumi::Document;
using document_text::str;

auto number_json(double value) -> std::string {
  char buffer[64];
  const auto [end, error] =
      std::to_chars(buffer, buffer + sizeof buffer, value);
  if (error != std::errc()) throw std::runtime_error("cannot print number");
  std::string out(buffer, end);
  if (out.find_first_of(".eE") == std::string::npos) out += ".0";
  return out;
}

// Decimal or exponent notation can still denote an exact int64.
auto integral_json(std::string_view raw) -> std::optional<std::string> {
  const bool negative = raw.starts_with("-");
  if (negative) raw.remove_prefix(1);
  const auto exponent_at = raw.find_first_of("eE");
  const auto mantissa = raw.substr(0, exponent_at);
  const auto dot = mantissa.find('.');
  std::string digits(mantissa);
  if (dot != std::string_view::npos) digits.erase(dot, 1);
  const auto first = digits.find_first_not_of('0');
  if (first == std::string::npos) return "0";
  digits.erase(0, first);

  int exponent = 0;
  if (exponent_at != std::string_view::npos) {
    auto suffix = raw.substr(exponent_at + 1);
    if (suffix.starts_with("+")) suffix.remove_prefix(1);
    const auto [end, error] =
        std::from_chars(suffix.data(), suffix.data() + suffix.size(), exponent);
    if (error != std::errc() || end != suffix.data() + suffix.size())
      return std::nullopt;
  }
  const auto fraction =
      dot == std::string_view::npos ? 0 : mantissa.size() - dot - 1;
  const auto shift =
      static_cast<int64_t>(exponent) - static_cast<int64_t>(fraction);
  if (shift < 0) {
    if (static_cast<uint64_t>(-shift) >= digits.size()) return std::nullopt;
    const auto end = digits.size() - static_cast<size_t>(-shift);
    if (digits.find_first_not_of('0', end) != std::string::npos)
      return std::nullopt;
    digits.resize(end);
  } else {
    if (shift > 19 || digits.size() + static_cast<size_t>(shift) > 19)
      return std::nullopt;
    digits.append(static_cast<size_t>(shift), '0');
  }
  if (negative) digits.insert(digits.begin(), '-');
  int64_t value = 0;
  const auto [end, error] =
      std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (error != std::errc() || end != digits.data() + digits.size())
    return std::nullopt;
  return std::to_string(value);
}
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

auto param_fbs_type(ParamType type) -> std::string {
  for (const auto& info : kParamTypes)
    if (type == info.type) return info.fbs;
  throw std::logic_error("unknown param type");
}

auto normalize_param_value(ParamType type, std::string_view raw)
    -> std::string {
  const auto captured = schema::capture(raw);
  const std::string text = schema::text(captured);
  const std::string_view value = text;
  const std::string_view name = param_type_name(type);
  if (name.starts_with("[")) {
    const auto element =
        *parse_param_type(std::string(name.substr(1, name.size() - 2)));
    std::vector<std::string> values;
    for (const auto& item : json::Array(value))
      values.push_back(normalize_param_value(element, item));
    return json::Encode(values);
  }
  switch (type) {
    case ParamType::Bool:
      if (value == "true" || value == "false") return text;
      break;
    case ParamType::String:
      if (value.starts_with("\"")) return json::Quote(json::String(value));
      break;
    case ParamType::Int64:
      if (!value.empty() && (value.front() == '-' ||
                             (value.front() >= '0' && value.front() <= '9')))
        if (auto result = integral_json(value)) return *result;
      break;
    case ParamType::Double: {
      double number = 0;
      const auto [end, error] =
          std::from_chars(value.data(), value.data() + value.size(), number);
      if (error == std::errc() && end == value.data() + value.size() &&
          std::isfinite(number))
        return number_json(number);
      break;
    }
    default:
      break;
  }
  throw std::runtime_error("expected " + std::string(name));
}

auto params_json(const std::vector<ParamDef>& params) -> std::string {
  json::Members values;
  for (const auto& param : params)
    if (param.data) values[param.name] = *param.data;
  return json::Encode(values);
}

auto params_buffer(const std::vector<ParamDef>& params) -> std::string {
  if (params.empty()) return {};
  std::vector<std::pair<std::string, std::string>> fields;
  for (const auto& param : params)
    fields.emplace_back(param.name, param_fbs_type(param.type));
  std::ranges::sort(fields);
  std::string error;
  const auto type =
      types::compile(types::params_schema(fields), "parameters.Params", &error);
  if (!type) throw std::runtime_error("params schema: " + error);
  std::string bytes;
  if (!types::json_to_flatbuffer(**type, params_json(params), &bytes, &error,
                                 /*force_defaults=*/true))
    throw std::runtime_error("params: " + error);
  return bytes;
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
    parts.push_back(
        it->name + "=" +
        (it->type == ParamType::String ? json::String(*it->data) : *it->data));
    it->sweep.reset();
  }
  for (auto it = parts.rbegin(); it != parts.rend(); ++it)
    member.label += (member.label.empty() ? "" : ", ") + *it;
  return member;
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
    -> std::vector<types::TypeRef> {
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
  std::vector<types::TypeRef> result;
  for (const auto& [_, candidate] : candidates) result.push_back(candidate);
  return result;
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
    if (d.symbol()) s.symbol = d.symbol()->str();
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
        n.input_defs.push_back({name});
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
    boolean(d.retainArtifact(), s.retain_artifact);
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
      const auto* v = &value;
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
    if (const auto* system = d.system()) {
      const auto value = system->str();
      const auto* v = &value;
      if (!remote && *v == native_system())
        s.system.clear();
      else if (*v == "x86_64-linux" || *v == "aarch64-linux")
        s.system = *v;
      else
        n.diags.push_back(diag(
            n.path, "invalid_setting",
            "system must be the native system, x86_64-linux or aarch64-linux"));
    }
    if (const auto* v = d.artifactOutput()) s.artifact_output = v->str();
    const auto* folders = grants ? grants->hostPaths() : nullptr;
    if (folders) {
      std::set<std::string> names;
      for (const auto* f : *folders) {
        const auto path_value = f->path()->str();
        const auto name_value = f->name()->str();
        const auto* path = &path_value;
        const auto* name = &name_value;
        if (path->empty() || path->front() != '/' || !valid_id(*name)) {
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
        s.grants.host_paths.push_back({*path, *name, f->write().value_or(false)});
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
    const auto rel = n.def_dir + "params.nix";
    auto sweeps = json::Object(
        def->card().sweeps() ? def->card().sweeps()->string_view() : "{}");
    const auto invalid = [&](const std::string& message) {
      n.diags.push_back(diag(n.path, "invalid_param", message, true, rel));
    };
    if (doc_.sources.contains(rel) && !read_params_) return;
    try {
      if (doc_.sources.contains(rel)) {
        if (doc_.sources.at(rel).size > kMaxDocumentJsonBytes)
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
            invalid("invalid declaration for param " + name);
            continue;
          }
          ParamDef param{name, *type, entry.at("value"), std::nullopt};
          if (const auto sweep = sweeps.extract(name); !sweep.empty()) {
            try {
              if (n.kind != CardType::Input)
                throw std::runtime_error("a sweep needs an Input Card param");
              std::vector<std::string> members;
              for (const auto& member : json::Array(sweep.mapped()))
                members.push_back(normalize_param_value(*type, member));
              param.sweep = std::move(members);
            } catch (const std::exception& error) {
              invalid("param " + name + ": " + error.what());
            }
          }
          n.settings.params.push_back(std::move(param));
        }
      }
      for (const auto& [name, _] : sweeps)
        invalid("undeclared sweep param " + name);
      n.params_nix_json = params_json(n.settings.params);
      if (value_sweep_size(n.settings) > kMaxValueSweepSize)
        invalid("the value sweep exceeds 256 combinations per trigger");
      if (n.kind == CardType::Code)
        n.run_params_buffer = params_buffer(n.settings.params);
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
      n.settings.retain_artifact = project.retainArtifact().value_or(false);
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
          // A connected start input triggers the Card on arrival.
          // An unconnected Input Card is a source that fires once per run.
          n.input_defs = {{"start"}};
          load_catalog(n);
          break;
        case CardType::Trigger:
          n.outputs.push_back(
              {"out", PortKind::FlatBuffer, false, types::empty_type()});
          break;
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
          n.inputs.push_back({port->name, PortKind::FlatBuffer, false, nullptr});
        for (const auto& port : b->outputs)
          n.outputs.push_back(
              {port->name, PortKind::FlatBuffer, false, nullptr});
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
        node.diags.push_back(diag(node.path, "invalid_type", "collect type: " + error));
    }
    for (size_t i = 0; i < res_->nodes.size(); ++i) {
      auto& node = res_->nodes[i];
      if (node.kind != CardType::Merge) continue;
      auto& output = node.outputs.front();
      const auto candidates = res_->merge_candidates(i, output.name);
      if (candidates.size() == 1) output.type = candidates.front();
      std::set<std::string> names;
      for (const auto& type : candidates) {
        const auto& full = type->root_type;
        const auto name = full.substr(full.find_last_of('.') + 1);
        names.insert(name);
      }
      if (names.size() != candidates.size()) {
        names.clear();
        for (const auto& type : candidates) names.insert(type->root_type);
      }
      output.merge_member_types.assign(names.begin(), names.end());
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

  void finish_checks() {
    for (size_t i = 0; i < res_->nodes.size(); ++i) {
      Node& n = res_->nodes[i];
      if (n.is_virtual) continue;
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

  auto exports_producer(size_t source) const -> std::optional<size_t> {
    std::set<size_t> visited;
    while (visited.insert(source).second) {
      switch (res_->nodes[source].kind) {
        case CardType::Code:
          return source;
        case CardType::Collect: {
          const auto& incoming = res_->incoming[source];
          const auto upstream = incoming.find("in");
          if (upstream == incoming.end()) return std::nullopt;
          source = upstream->second;
          break;
        }
        default:
          return std::nullopt;
      }
    }
    return std::nullopt;
  }

  auto resolve_nix_inputs() -> void {
    for (const auto& edge : res_->edges) {
      auto& node = res_->nodes[edge.to];
      if (node.kind != CardType::Code) continue;
      const auto* port = node.input(edge.to_port);
      if (!port) continue;
      auto producer = exports_producer(edge.from);
      if (producer) {
        const auto allowed = res_->nix_upstream(edge.to);
        if (std::ranges::find(allowed, *producer) == allowed.end())
          producer.reset();
      }
      node.nix_ports.emplace(edge.to_port, NixInput{*port, producer});
      if (producer)
        node.nix_references.push_back(
            {res_->nodes[*producer].path, edge.to_port});
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
                                          const SourceReader& read,
                                          const ParamsReader& read_params) {
  return Builder(doc, read, read_params).run();
}

}  // namespace tsugumi
