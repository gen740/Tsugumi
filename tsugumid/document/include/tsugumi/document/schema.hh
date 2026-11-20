// Project definition JSON contracts.
#pragma once

#include "tsugumi/util/json.hh"

namespace tsugumi::schema {
struct Endpoint {
  Opt<"card", v::String> card;
  Opt<"port", v::String> port;
};

struct Connection {
  Opt<"from", Obj<Endpoint>> from;
  Opt<"to", Obj<Endpoint>> to;
};

struct Boundary {
  Opt<"inputs", v::Mapping<v::String, Obj<Endpoint>>> inputs;
  Opt<"outputs", v::Mapping<v::String, Obj<Endpoint>>> outputs;
};

// A Card on the canvas. Its id is its name and the folder of its own
// definition (definitions/<id>/), which holds every setting.
struct CardInstance {
  Opt<"id", v::String> id;
};

struct Graph {
  Opt<"cards", v::Array<Obj<CardInstance>>> cards;
  Opt<"connections", v::Array<Obj<Connection>>> connections;
};

// A Nix source outside nixpkgs that this project opts in to. Its Cards read it
// as inputs.sources.<name>. `type` says how it is read: "flake" for now, whose
// `ref` is locked (a commit and its narHash).
struct ExternalPackageOverride {
  Opt<"override", v::Mapping<v::String, Json>> arguments;
  Opt<"overrideAttrs", v::Mapping<v::String, Json>> attributes;
};

struct NixSource {
  Opt<"name", v::String> name;
  Opt<"type", v::String> type;
  Opt<"ref", v::String> ref;
  Opt<"overrides", Json> overrides;
};

struct Project {
  Opt<"name", Json> name;
  Opt<"pkgsPath", v::String> pkgs_path;
  Opt<"nixSources", v::Array<Obj<NixSource>>> nix_sources;
  // Additional literal project-relative files or folders kept in versions.
  Opt<"versionPaths", v::Array<v::String>> version_paths;
  // Host policy, carried without affecting execution.
  Opt<"agentExclude", v::Array<v::String>> agent_exclude;
  // Finished runs are also kept in the project (.tsugumi/runs.sqlite); default
  // true.
  Opt<"canvas", Obj<Graph>> canvas;
  // The ports this project has as a Card in Card (an external of another
  // project): outer port name -> a port of one of its Cards.
  Opt<"boundary", Obj<Boundary>> boundary;
};

// ---- definitions/<id>/_card.json
// ---------------------------------------------

struct ParamDef {
  Opt<"name", v::String> name;
  Opt<"type", v::String> type;
  Opt<"data", Json> data;
  // An Input Card's direct value sweep: one invocation per value, instead of
  // data. A list-typed param sweeps lists; its data stays one vector value.
  Opt<"sweep", v::Array<Json>> sweep;
  Opt<"nix", v::Bool> nix;
};

struct InputDef {
  Opt<"name", v::String> name;
  Opt<"optional", v::Bool> optional;
};

// A Fork's output port; other Cards' outputs come from their schema.
struct OutputDef {
  Opt<"name", v::String> name;
};

// A folder the worker may read (Experimental): a folder of the Mac, seen in
// the worker as /host/<name>.
struct HostFolder {
  Opt<"path", v::String> path;  // absolute path on the Mac
  Opt<"name", v::String> name;  // folder name under /host (an id)
  Opt<"write", v::Bool> write;  // default false: read-only
};

struct ExecutionResourcesJson {
  Opt<"cpuMillis", v::Uint64> cpu_millis;
  Opt<"memoryBytes", v::Uint64> memory_bytes;
};

struct WorkerConditionJson {
  t::String<"language"> language;
  t::String<"expression"> expression;
};

struct CardDefinition {
  Opt<"external", v::String>
      external;  // externals/<external>/ for Cards in Card
  Opt<"symbol", v::String> symbol;
  Opt<"language", v::String> language;
  Opt<"template", v::String> template_name;
  Opt<"params", v::Array<Obj<ParamDef>>> params;
  Opt<"inputs", v::Array<Obj<InputDef>>> inputs;
  Opt<"outputs", v::Array<Obj<OutputDef>>> outputs;  // a Fork's
  // When the Card runs: "allInputs" (every required input has a value, the
  // default) or "eachInput" (once per arriving value).
  Opt<"trigger", v::String> trigger;
  Opt<"maxRuns", v::Uint64> max_runs;
  Opt<"timeoutSeconds", Json> timeout_seconds;  // positive integer or null
  Opt<"buildTimeoutSeconds", Json>
      build_timeout_seconds;  // positive integer or null
  Opt<"recordIO", v::Bool> record_io;
  Opt<"retainArtifact", v::Bool> retain_artifact;
  Opt<"artifactOutput", v::String> artifact_output;
  Opt<"sandbox", v::Bool> sandbox;
  Opt<"reuseWorker", v::Bool>
      reuse_worker;  // Keep worker state across invocations and runs. Default
                     // false.
  Opt<"network", v::Bool> network;  // Experimental, default false
  Opt<"executionService", v::String> execution_service;
  Opt<"workerCondition", Obj<WorkerConditionJson>> worker_condition;
  Opt<"executionResources", Obj<ExecutionResourcesJson>> execution_resources;
  // The Nix system the Card is built for and runs as: "x86_64-linux" or
  // "aarch64-linux". Default: this machine's. Another one runs emulated.
  Opt<"system", v::String> system;
  // How a figure on the canvas follows the Card's results: "update" (the
  // default: the last figure, dimmed while the Card or one upstream runs) or
  // "live" (each revision of a running Card replaces the last one in place).
  // Carried opaquely; only clients interpret it.
  Opt<"displayMode", Json> display_mode;
  Opt<"hostFolders", v::Array<Obj<HostFolder>>> host_folders;  // Experimental
};

}  // namespace tsugumi::schema
