// Derived view of a Document: nested graphs flattened to full Card paths,
// effective settings and resolved port types. Everything here is derived from
// the document and never written back to it.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "tsugumi/document/document.hh"
#include "tsugumi/document/types.hh"
#include "tsugumi/document/worker_condition.hh"
#include "tsugumi/util/system.hh"

namespace tsugumi {

enum class Template {
  Code,
  Trigger,
  Input,
  Branch,
  Merge,
  Collect,
  Subgraph,
  Draft
};
const char* template_name(Template t);
// What a definition is; no definition at all is a Draft.
Template template_of(const DefinitionDoc* def);

enum class ParamType {
  Bool,
  Int64,
  UInt64,
  Double,
  String,
  BoolArray,
  Int64Array,
  UInt64Array,
  DoubleArray,
  StringArray,
};
std::optional<ParamType> parse_param_type(const std::string& s);
const char* param_type_name(ParamType t);
std::string param_fbs_type(ParamType t);

struct ParamDef {
  std::string name;
  ParamType type = ParamType::String;
  std::optional<hosq::json::GenericDom<>>
      data;  // effective value (validated against type)
  // An Input Card's value sweep (exclusive with data): one execution batch
  // member per value, in order, and the Cartesian product across swept params.
  std::optional<std::vector<hosq::json::GenericDom<>>> sweep;
  bool nix = false;
};

struct HostFolder {
  std::string path;  // on the Mac
  std::string name;  // /host/<name> in the worker
  bool write = false;
};

inline constexpr char kLocalExecutionService[] = "local";

struct Settings {
  std::string name;  // the Card id
  std::string symbol;
  std::string
      language;  // "", cpp, python, go, rust, csharp, bash, r, julia, ruby
  bool any_mode = false;  // trigger "eachInput"
  uint64_t max_runs = 1;
  std::optional<uint64_t> timeout_seconds;        // nullopt = no deadline
  std::optional<uint64_t> build_timeout_seconds;  // nullopt = no deadline
  bool record_io = false;
  bool retain_artifact = false;
  std::optional<std::string> artifact_output;
  bool sandbox = true;
  bool reuse_worker = false;
  bool network = false;
  std::string execution_service = "auto";
  std::shared_ptr<const worker_condition::Condition> worker_condition;
  std::optional<ExecutionResources> execution_resources;
  std::string system;  // "" = the execution service's native system
  std::vector<HostFolder> host_folders;
  std::vector<ParamDef> params;  // with their values in data
};

struct InputDef {
  std::string name;
  bool optional = false;
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

struct NixSourceRef {
  std::string name, type, ref;
  std::string overrides = "{}";
};

struct CardNixReference {
  CardPath target;
  std::string name;
  bool valid = false;
};

struct Node {
  CardPath path;
  Template kind = Template::Code;
  // The Card's definition folder, project relative ("definitions/<id>/", or
  // "externals/<p>/definitions/<id>/" inside a Card in Card).
  std::string def_dir;
  std::string project_dir;  // "" or "externals/<p>/": the project the Card is
                            // written in
  std::string pkgs_path;    // of the project the Card is written in
  std::vector<NixSourceRef>
      nix_sources;  // what that project opts in to (tsugumi.json nixSources)
  std::vector<CardNixReference> nix_references;
  std::map<std::string, size_t>
      nix_inputs;  // destination port -> guaranteed Code producer
  std::optional<schema::Boundary>
      boundary;          // Card in Card: its external's ports
  std::string external;  // Card in Card: "externals/<p>/" (project relative)
  Settings settings;
  std::vector<InputDef> input_defs;
  std::vector<types::PortType> inputs;
  std::vector<types::PortType> outputs;
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
  std::vector<std::map<std::string, std::vector<size_t>>> incoming;
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

std::shared_ptr<const Resolution> resolve(const Document& doc,
                                          const SourceReader& read);

// Checks that a captured JSON value has the param type (no implicit
// conversion, integers in range); returns error text.
std::optional<std::string> check_param_value(ParamType t,
                                             const hosq::json::GenericDom<>& v);
// Decodes an array-of-strings param.

}  // namespace tsugumi
