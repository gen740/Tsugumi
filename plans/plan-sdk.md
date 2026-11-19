# SDK rebuild plan (decided 2026-11-18)

Goal: adding or changing a Card builds only that Card's share, and every language's Card has the same shape.

## 1. The Card's shape (every language)

- A Card is one `main.so` exporting one fixed symbol, `tsugumi_main`.
- `main.so` only takes the inputs and params FlatBuffers and returns outputs of a fixed type: inputs are `_inputs.fbs` (file and folder ports arrive as paths), outputs are `_outputs.fbs`.
- Side effects (displays, logs, cancellation, deadlines, failure records) are functions on the worker side that the Card calls; `main.so` holds none of them.
- A Card cannot call another Card.
- Change from today: outputs leave as the return value (one `Outputs`), not port by port through `emit_output`; the side-effect functions are a separate table. The ABI number goes up; no compatibility with the old shape.

## 2. Layers

| Layer | Contents | Compiles | Rebuilt when |
| --- | --- | --- | --- |
| worker (with tsugumid) | the Card host and the side-effect functions (displays, logs, cancellation, deadlines, failures, typed JSON conversion) | yes | tsugumid is rebuilt |
| per language (once per SDK and language version) | the start-up code (starts Python, Ruby, R, Julia, C# and loads the Card's code: today's `python_main.c` and the like as compiled static libraries); the language module (`tsugumi.py`, `tsugumi.rb`, `tsugumi.R`, `tsugumi.jl`, … wrapping the side-effect and display calls); the compiled common parts of Go, Rust and C++ (the SDK's Go package, Rust crate, flatbuffers libraries, `rust_host.c`, …; for Go also a build cache including the standard library) | yes, once | that language's SDK files or its version change |
| views (shared) | built-in views and the WebAssembly view base, as today | yes, once | view sources change |
| per contract | generated from `_inputs.fbs`, `_outputs.fbs`, `_params.fbs`: each language's typed bindings and the source of `main.so`'s entry | no (generation only) | a port's or param's type changes |
| Card environment (per lock) | Python packages, gems, R and Julia packages, cargo, Go and NuGet dependencies, as today | only for native extensions | the lock or the packages change |
| per Card | compile the generated entry, link it with the language layer and the Card's code into `main.so`. C++, Go, Rust and C# also compile the Card's own code here; Python and the like only write the Card source's location into the entry | only the entry and the Card | the Card's source changes |

Nix rules:

- Each layer takes in only the files it reads, with Nix path types and `lib.fileset` (no `"${sdkRoot}/…"` or `sdkRoot + "/…"` strings).
- The per-Card layer uses `runCommandCC`, not `stdenv.mkDerivation`.

## 3. Removed

- The per-Card `*-runtime` of Python, Ruby, R, Julia and C# (Card locations baked in with `-D`, compiled per Card).
- String paths into the SDK, which make any SDK change rebuild every Card.
- Passing the whole generated folder to the SDK (it captures `params-nix.json` and `sources.json`, so a `nix: true` param value change rebuilds the bindings). Only the four contract files are passed.
- The `.bfbs` files tsugumid writes (nobody reads them); they are made from `.fbs` where needed.
- Running `go test` and `cargo test` on every build: only when the Card has test files.
- Two JSON conversions: Ruby compiles `json_face.cc` per Card while R and Julia use the worker's conversion (`input_json` and the like). Everything uses the worker's conversion and `json_face.cc` goes.

## 4. Decided

1. Outputs are returned as one `Outputs` value; displays can still be sent any number of times while running.
2. JSON conversion is the worker's.

## 5. Order

1. Inputs: path types and `lib.fileset`, only the four contract files, remove unused `.bfbs`. No ABI change.
2. Compile the language layer once (start-up code, Go and Rust common parts); per-Card layer as `runCommandCC`.
3. ABI: outputs as the return value, a separate side-effect table; one entry codegen across languages; remove `json_face.cc`.
4. Tests only when the Card has test files.
