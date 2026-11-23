// Derived view of a Document: nested graphs flattened to full Card paths,
// effective settings and resolved port types. Everything here is derived from
// the document and never written back to it.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "tsugumi/document/document.hh"
#include "tsugumi/document/types.hh"
#include "tsugumi/document/worker_condition.hh"
#include "tsugumi/util/system.hh"

namespace tsugumi {

enum class CardType {
  Code,
  Trigger,
  Input,
  Branch,
  Fork,
  Merge,
  Collect,
  Subgraph,
  Draft
};
// The _card.json "type" value of each kind.
const char* card_type_name(CardType t);
std::optional<CardType> parse_card_type(std::string_view name);
// The type a parsed definition states (parse_card_json has checked it).
CardType card_type_of(const DefinitionDoc& def);
// A Fork's output names: its declared outputs, or a and b without any.
std::vector<std::string> fork_outputs(const DefinitionDoc& def);

enum class ParamType {
  Bool,
  Int64,
  Double,
  String,
  BoolArray,
  Int64Array,
  DoubleArray,
  StringArray,
};
std::optional<ParamType> parse_param_type(const std::string& s);
const char* param_type_name(ParamType t);
std::string param_fbs_type(ParamType t);

struct ParamDef {
  std::string name;
  ParamType type = ParamType::String;
  std::optional<std::string> data;  // value as canonical JSON
  std::optional<std::vector<std::string>> sweep;
};

auto normalize_param_value(ParamType type, std::string_view json)
    -> std::string;
auto params_json(const std::vector<ParamDef>& params) -> std::string;
auto params_buffer(const std::vector<ParamDef>& params) -> std::string;

struct HostFolder {
  std::string path;  // on the Mac
  std::string name;  // /host/<name> in the worker
  bool write = false;
};

struct CardGrants {
  bool network = false;
  bool gui = false;
  std::vector<HostFolder> host_paths;
};

inline constexpr char kLocalExecutionService[] = "local";

struct Settings {
  std::string name;  // the Card id
  std::string symbol;
  uint64_t max_runs = 1;
  std::optional<uint64_t> timeout_seconds;        // nullopt = no deadline
  std::optional<uint64_t> build_timeout_seconds;  // nullopt = no deadline
  bool record_logs = true;
  uint64_t log_limit_bytes = kDefaultLogLimitBytes;
  uint64_t output_limit_bytes = kDefaultOutputLimitBytes;
  uint64_t output_limit_entries = kDefaultOutputLimitEntries;
  bool record_io = false;
  SandboxOptions sandbox;
  CardGrants grants;
  bool reuse_worker = false;
  std::string execution_service = "auto";
  std::shared_ptr<const worker_condition::Condition> worker_condition;
  std::string system;  // "" = the execution service's native system
  // Card secret names, in declaration order: [a-z][a-z0-9_]{0,63}, each the
  // name of the environment variable that carries its value.
  std::vector<std::string> secrets;
  std::vector<ParamDef> params;  // with their values in data
};

struct InputDef {
  std::string name;
};

// An Input Card's value sweep. Members follow the param order, with the last
// swept param varying fastest; a Card without a sweep has one member.
inline constexpr uint64_t kMaxValueSweepSize = 256;
bool has_value_sweep(const Settings& settings);
uint64_t value_sweep_size(const Settings& settings);
struct ValueSweepMember {
  std::vector<ParamDef> params;  // every param, with this member's data
  std::string label;  // "name=value, ..." of the swept params; "" without
};
ValueSweepMember value_sweep_member(const Settings& settings, uint64_t index);

struct NixPathEntry {
  std::string name;
  std::string url;
};

struct NixInput {
  types::PortType type;
  std::optional<size_t> exports_node;
};

struct CardNixReference {
  CardPath target;
  std::string name;
};

struct Node {
  CardPath path;
  CardType kind = CardType::Code;
  // The Card's definition folder, project relative ("definitions/<id>/", or
  // "externals/<p>/definitions/<id>/" inside a Card in Card).
  std::string def_dir;
  std::string project_dir;  // "" or "externals/<p>/": the project the Card is
                            // written in
  std::vector<NixPathEntry> nix_path;  // declarations in project_dir
  std::vector<CardNixReference> nix_references;
  std::map<std::string, NixInput> nix_ports;
  std::string params_nix_json = "{}";
  std::string run_params_buffer;
  std::optional<Tsugumi::Document::BoundaryT>
      boundary;          // Card in Card: its external's ports
  std::string external;  // Card in Card: "externals/<p>/" (project relative)
  Settings settings;
  std::vector<InputDef> input_defs;
  std::vector<types::PortType> inputs;
  std::vector<types::PortType> outputs;
  std::map<std::string, uint8_t> merge_tags;
  std::vector<Diag> diags;       // structure diagnostics
  std::vector<Diag> exec_diags;  // additional diagnostics for executable mode
  bool is_virtual = false;       // subgraph container, never executed

  const types::PortType* input(const std::string& n) const;
  const types::PortType* output(const std::string& n) const;
};

struct Edge {
  size_t from;
  std::string from_port;
  size_t to;
  std::string to_port;
};

struct Resolution {
  std::vector<Node> nodes;
  std::map<std::string, size_t> index;  // path_key -> node
  std::vector<Edge> edges;              // between executable nodes only
  std::vector<std::map<std::string, size_t>> incoming;
  std::vector<std::vector<size_t>> outgoing;
  std::vector<Diag> diags;
  // Boundary aliases produced by the same leaf resolution as executable wires.
  std::map<std::tuple<std::string, std::string, bool>,
           std::pair<size_t, std::string>>
      endpoints;
  auto endpoint(const CardPath& card, const std::string& port,
                bool output) const
      -> std::optional<std::pair<size_t, std::string>>;
  // Per node: the nodes that have always run before it fires, whichever path
  // its inputs arrived on (sorted). The basis for Nix upstream references.
  std::vector<std::vector<size_t>> guaranteed;
  // Code Cards a node's default.nix may reference: guaranteed upstream Code
  // Cards whose own guarantees do not include the node (no cycles).
  std::vector<size_t> nix_upstream(size_t node) const;

  const Node* find(const CardPath& p) const;
  std::vector<Diag> all_diagnostics(bool executable) const;
};

using SourceReader =
    std::function<std::optional<std::string>(const std::string& rel)>;
using ParamsReader = std::function<std::string(const std::string& def_dir,
                                               std::string_view text)>;

std::shared_ptr<const Resolution> resolve(const Document& doc,
                                          const SourceReader& read,
                                          const ParamsReader& read_params = {});

}  // namespace tsugumi
