// Derived view of a Document: nested graphs flattened to full Card paths,
// effective settings and resolved port types. Everything here is derived from
// the document and never written back to it.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "document.hh"
#include "types.hh"

namespace tsugumi {

enum class Template { Code, Trigger, Input, Switch, Fork, Branch, Merge, Logger, Subgraph, Draft };
const char* template_name(Template t);
// What a definition is; no definition at all is a Draft.
Template template_of(const DefinitionDoc* def);

enum class ParamType {
  Bool, Int64, UInt64, Double, String,
  BoolArray, Int64Array, UInt64Array, DoubleArray, StringArray,
};
std::optional<ParamType> parse_param_type(const std::string& s);
const char* param_type_name(ParamType t);
std::string param_fbs_type(ParamType t);

struct ParamDef {
  std::string name;
  ParamType type = ParamType::String;
  std::optional<hosq::json::GenericDom<>> data;  // effective value (validated against type)
  bool nix = false;
};

struct HostFolder {
  std::string path;  // on the Mac
  std::string name;  // /host/<name> in the worker
  bool write = false;
};

struct Settings {
  std::string name;  // the Card id
  std::string symbol;
  std::string language;  // "", cpp, python, go, bash, r, julia
  bool any_mode = false;  // trigger "eachInput"
  uint64_t max_runs = 1;
  std::optional<uint64_t> timeout_seconds;        // nullopt = no deadline
  std::optional<uint64_t> build_timeout_seconds;  // nullopt = no deadline
  bool record_io = false;
  bool retain_artifact = false;
  std::optional<std::string> artifact_output;
  bool filesystem = false;
  bool sandbox = true;
  bool reuse_worker = true;
  bool network = false;
  bool markdown = false;  // logger: shows the text it receives as Markdown
  std::string system;     // "" = native_system(); another one runs emulated (binfmt)
  std::vector<HostFolder> host_folders;
  std::vector<ParamDef> params;  // with their values in data
};

// The Nix system of this machine: what a Card is built for unless its
// settings name another.
constexpr const char* native_system() {
#if defined(__aarch64__) && defined(__APPLE__)
  return "aarch64-darwin";
#elif defined(__aarch64__)
  return "aarch64-linux";
#elif defined(__x86_64__) && defined(__APPLE__)
  return "x86_64-darwin";
#else
  return "x86_64-linux";
#endif
}

struct InputDef {
  std::string name;
  bool optional = false;
};

struct NixSourceRef {
  std::string name, type, ref;
  std::string overrides = "{}";
};

struct Node {
  CardPath path;
  Template kind = Template::Code;
  // The Card's definition folder, project relative ("definitions/<id>/", or
  // "externals/<p>/definitions/<id>/" inside a Card in Card).
  std::string def_dir;
  std::string project_dir;                    // "" or "externals/<p>/": the project the Card is written in
  std::string pkgs_path;                      // of the project the Card is written in
  std::vector<NixSourceRef> nix_sources;      // what that project opts in to (tsugumi.json nixSources)
  std::optional<schema::Boundary> boundary;  // Card in Card: its external's ports
  std::string external;                      // Card in Card: "externals/<p>/" (project relative)
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
  std::vector<Edge> edges;               // between executable nodes only
  std::vector<Diag> diags;
  // Per node: the nodes that have always run before it fires, whichever path
  // its inputs arrived on (sorted). The basis for Nix upstream references.
  std::vector<std::vector<size_t>> guaranteed;
  // Code Cards a node's default.nix may reference: guaranteed upstream Code
  // Cards whose own guarantees do not include the node (no cycles).
  std::vector<size_t> nix_upstream(size_t node) const;

  const Node* find(const CardPath& p) const;
  std::vector<Diag> all_diagnostics(bool executable) const;
};

using SourceReader = std::function<std::optional<std::string>(const std::string& rel)>;

std::shared_ptr<const Resolution> resolve(const Document& doc, const SourceReader& read);

// Checks that a captured JSON value has the param type (no implicit
// conversion, integers in range); returns error text.
std::optional<std::string> check_param_value(ParamType t, const hosq::json::GenericDom<>& v);
// Decodes an array-of-strings param.
std::vector<std::string> string_array_param(const hosq::json::GenericDom<>& v);

}  // namespace tsugumi
