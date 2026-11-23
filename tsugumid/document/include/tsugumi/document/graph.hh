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

struct ParamDef {
  std::string name;
  ParamType type = ParamType::String;
  std::optional<std::string> data;  // value as canonical JSON
};

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
  bool retain_artifact = false;
  std::optional<std::string> artifact_output;
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

struct NixPathEntry {
  std::string name;
  std::string url;
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
  std::optional<std::string> generated_inputs;
  std::optional<Tsugumi::Document::BoundaryT>
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
  // Concrete producer types reachable through a Merge output, by schema ID.
  auto merge_candidates(size_t node, const std::string& port) const
      -> std::vector<types::TypeRef>;

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
