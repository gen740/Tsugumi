// SDK worker manifest JSON contract.
#pragma once

#include "tsugumi/util/json.hh"

namespace tsugumi::schema {
// The manifest written by the SDK's tsugumi.manifest (read only). `run` is the
// Card body: a package (store path; lib/main.so or lib/tsugumi-runtime naming an SDK artifact) or a
// bash script, never both.
struct ManifestRun {
  Opt<"package", v::String> package;
  Opt<"bash", v::String> bash;
  Opt<"worker", v::String> worker;  // a package of another system: the worker built for it
};

struct CardManifest {
  Opt<"run", Obj<ManifestRun>> run;
  Opt<"runs", v::Array<Obj<ManifestRun>>> runs;
  Opt<"shell", v::String> shell;
  Opt<"path", v::String> path;
  Opt<"preRun", v::String> pre_run;
  Opt<"postRun", v::String> post_run;
  Opt<"views", v::String> views;  // JSON: view module name -> Nix store directory
};


}  // namespace tsugumi::schema
