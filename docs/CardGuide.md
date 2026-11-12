# Your first Card

Make a Card that just multiplies the number it receives, run it, and try other conditions.
After making it in Python, replace the same Card with bash, C++, R and Julia.

The full specification is in [agent/Reference/card-guide.md](../agent/Reference/card-guide.md), the same document the app's Agent reads.

## What you will make

```text
start ──out→ settings ──number→ multiply
```

| Card | Kind | What it does |
| --- | --- | --- |
| `start` | Start (trigger) | Where the run begins |
| `settings` | Input value (input) | Sends a number you can edit on the Canvas |
| `multiply` | Processing (Python) | Multiplies the number it receives by `factor`, outputs the result and displays it |

When it is done, the Canvas looks like this. Below are the same Cards as on the real screen, and they run without installing anything. Press ▶ Run to watch the flow, and click `multiply` to read the Python code on this page (web version only).

<div data-player="card-guide"></div>

## Making it on screen

1. Make a project with "New Project…" on the home screen.
2. Right-click the Canvas and, from "Add Card", place "Start", "Input value" and "Processing (Python)", naming them `start`, `settings` and `multiply`.
3. Draw a wire from the output of `start` to `settings`, and from the output of `settings` to `multiply`.
4. Open each Card (the `s` key or "Settings" in the context menu) and make it the same as the files in the next section.

A Card's settings look like this. Switch between "Source", "Ports" and "Nix Definition" in the list on the left to edit, and "Apply" puts the change into the Workflow.

![Editing the source code in a Card's settings](assets/screenshots/card-source.png)

A Card is ordinary files in the project folder, so writing them directly in an editor is the same.

## The files

The whole project:

`tsugumi.json`

```json
{
  "name": "Your first Card",
  "pkgsPath": "tsugumi:default",
  "databaseConnections": [],
  "canvas": {
    "cards": [{"id": "start"}, {"id": "settings"}, {"id": "multiply"}],
    "connections": [
      {"from": {"card": "start", "port": "out"}, "to": {"card": "settings", "port": "start"}},
      {"from": {"card": "settings", "port": "number"}, "to": {"card": "multiply", "port": "number"}}
    ]
  }
}
```

`definitions/start/_card.json`

```json
{"template": "trigger"}
```

### The input value Card

Write the output type in `_outputs.fbs`. The fields of `Outputs` are the output ports.
`params` have the same names as the fields of the table that is output.

`definitions/settings/_card.json`

```json
{"template": "input", "params": [{"name": "value", "type": "uint64", "data": 21}]}
```

`definitions/settings/_outputs.fbs`

```text
namespace hello;
table Number { value:ulong; }
table Outputs { number:Number (required); }
root_type Outputs;
```

For a File output, the `value` parameter names a saved project source. A path such
as `data/events.fits` is relative to the input Card's own definition folder;
`project:definitions/shared/data/events.fits` explicitly uses the owning
project's root to name a saved source in another Card. Inside an
external project, these bases stay within that external project. Absolute paths
and `..` components are rejected. The file is copied into the saved run before
it is passed downstream, so later source edits do not change that run's file.

### The processing Card (Python)

Write the names of the input ports in `inputs`. Input types are not written: they are derived from the outputs of the connected Cards.

`definitions/multiply/_card.json`

```json
{
  "language": "python",
  "inputs": [{"name": "number"}],
  "params": [{"name": "factor", "type": "uint64", "data": 2}]
}
```

`definitions/multiply/_outputs.fbs`

```text
namespace hello;
table Result { value:ulong; }
table Outputs { result:Result (required); }
root_type Outputs;
```

A Card's summary goes in `_meta.json`. It appears on the Canvas and in the Card's settings, so write briefly what the Card does.

`definitions/multiply/_meta.json`

```json
{
  "description": "Multiply number by factor"
}
```

The environment goes in `default.nix`. To use `numpy` and the like, add `packages = ps: [ ps.numpy ];` to `pythonCard`.

A package that nixpkgs does not have, or a version it does not carry, comes from PyPI through a lock that belongs to the Card. Nothing is resolved when a Card is built: a Card uses PyPI packages only when its `default.nix` names both the lock and the hash of what the lock fetches.

1. Put a `pyproject.toml` in the Card's folder, beside `default.nix`:

   ```toml
   [project]
   name = "card"
   version = "0"
   requires-python = ">=3.11"
   dependencies = ["tabulate==0.9.0"]
   ```

2. Make `uv.lock` beside it. In the project's terminal (the lock is made by uv, which needs the network once):

   ```sh
   cd definitions/multiply
   nix shell nixpkgs#uv nixpkgs#python3 --command env UV_PYTHON_DOWNLOADS=never uv lock
   ```

3. Name the lock in `default.nix`, with a placeholder hash:

   ```nix
   run = inputs.tsugumi.pythonCard {
     inherit sdk;
     src = ./src;
     lock = ./uv.lock;
     hash = pkgs.lib.fakeHash;
   };
   ```

4. Run the Card once. The Card’s build log reports `hash mismatch ... got: sha256-...`. Replace `pkgs.lib.fakeHash` with that hash (`hash = "sha256-...";`) and run again.

From then on the Card is built from exactly those files, without the network. To change a dependency, edit `pyproject.toml`, repeat steps 2 to 4, and keep `pyproject.toml`, `uv.lock` and `default.nix` together in the Card's folder. A lock that no longer matches `pyproject.toml`, a lock without a hash and a hash that does not match are errors that say so; an old hash never lets the previous packages through.

- Only wheels are installed. A package that has no wheel for the Card's Python and system fails with a message; take such a package from nixpkgs with `packages`. `packages` and `lock` can be used together, and a locked package wins over one of the same name from nixpkgs.
- Compiled wheels differ per system, so their hash does too. Give one per system the Card is built for: `hash = { aarch64-linux = "sha256-..."; x86_64-linux = "sha256-..."; };`.
- A compiled wheel that loads a shared library beyond the C and C++ runtimes and zlib gets it from `libraries = [ pkgs.libGL ];`.

`definitions/multiply/default.nix`

```nix
{ pkgs, inputs, params }:
let
  sdk = inputs.tsugumi.sdk { languages = [ "python" ]; };
in
{
  run = inputs.tsugumi.pythonCard { inherit sdk; src = ./src; };
}
```

The body is `main(ctx)`. Import inputs from `inputs.<port>.<namespace>.<table>` and outputs from `outputs.<port>.<namespace>.<table>`.

`definitions/multiply/src/main.py`

```python
from inputs.number.hello.Number import Number
import outputs.result.hello.Result as result_out

def main(ctx):
    n = ctx.input("number", Number).Value()
    factor = ctx.params().Factor()
    b = ctx.builder()
    result_out.Start(b)
    result_out.AddValue(b, n * factor)
    ctx.output("result", b, result_out.End(b))
    ctx.display_text(f"{n} × {factor} = {n * factor}")
```

## Running it and checking

1. Press "Run" (⌘R) at the top of the Canvas. Only the first time takes a while, as Nix prepares the environment.
2. `multiply` displays `21 × 2 = 42`.
3. "Run History" in the context menu shows the time and logs of each stage (Nix evaluation, build, run, saving).

## Trying other conditions

- **Change only a parameter.** Set `factor` of `multiply` to 3 and run only `multiply` with "Run Only This Card (Last Inputs)" in the context menu. `settings` does not run, and with the saved input the result is `21 × 3 = 63`. `factor` is not passed to Nix, so nothing is rebuilt either.
- **Change something upstream.** After changing `value` of `settings`, run again from `start`. Partial runs use saved inputs, so an upstream change does not arrive until you run the whole ([Rerunning from the middle](Rationale.md#rerunning-from-the-middle)).
- **Go back to an earlier version.** Each run commits the changes to git. From "Version History…" (⌥⌘Y) in the Workflow menu, go back to an earlier version and start a different attempt ([Versions](Versions.md)).

## Writing the same Card in another language

As long as the input and output types are the same, what is inside can be replaced while the wires on the Canvas and the Cards around it stay as they are.

### Python, with plain objects

The generated Python classes also come in an object form (`NumberT`, `ResultT`, `ParamsT`): plain attributes named after the schema's fields (`photon_count` becomes `photonCount`), and `Pack` to build an output. `tsugumi.contract` gives an input or the params in that form without importing the class by name, and lists the Card's ports (`contract.ports()`, `contract.port("number")["kind"]`). It is meant for code shared by many Cards, which cannot know the port types when it is written.

`definitions/multiply/src/main.py` (Python objects)

```python
from outputs.result.hello.Result import ResultT
from tsugumi import contract

def main(ctx):
    n = contract.input_object(ctx, "number").value
    factor = contract.params_object(ctx).factor
    result = ResultT()
    result.value = n * factor
    b = ctx.builder()
    ctx.output("result", b, result.Pack(b))
    ctx.display_text(f"{n} × {factor} = {n * factor}")
```

### bash

Delete `src/`, set `language` to `bash`, and write a string in `run` of `default.nix`. Inputs are `$JSON_<PORT>` (the path of a JSON file); write the output as JSON to `$TSUGUMI_OUTPUT`.

`definitions/multiply/_card.json` (bash)

```json
{
  "language": "bash",
  "inputs": [{"name": "number"}],
  "params": [{"name": "factor", "type": "uint64", "data": 2, "nix": true}]
}
```

`definitions/multiply/default.nix` (bash)

```nix
{ pkgs, inputs, params }: {
  run = ''
    jq --argjson f ${toString params.factor} '{result: {value: (.value * $f)}}' "$JSON_NUMBER" > "$TSUGUMI_OUTPUT"
  '';
}
```

Here `"nix": true` is set and `factor` is read from the Nix expression. This param is passed to Nix, so changing it rebuilds the Card. To read params at run time instead, all of them are in `$TSUGUMI_PARAMS_JSON` (the JSON text).

For a display, write JSON to `$TSUGUMI_DISPLAY`: one of `{"markdown": "**bold**"}`, `text`, `table` (`columns` and `rows`), `image` (`path` and `alt`) or `module` (`name` and `data`). It is shown when the script ends normally.

### C++

The input and output ports become members of `Card::Input` and `Card::Output` as they are.

`definitions/multiply/_card.json` (C++)

```json
{
  "language": "cpp",
  "inputs": [{"name": "number"}],
  "params": [{"name": "factor", "type": "uint64", "data": 2}]
}
```

`definitions/multiply/default.nix` (C++)

```nix
{ pkgs, inputs, params }:
let
  sdk = inputs.tsugumi.sdk { languages = [ "cpp" ]; };
in
{
  run = inputs.tsugumi.cppCard { inherit sdk; src = ./src; };
}
```

`definitions/multiply/src/card.cc` (C++)

```cpp
#include <card.hh>

#include <string>

auto run(Card::Input& input) -> Card::Output {
  const auto n = input.number.value;
  const auto factor = Card::params().factor.value_or(1);
  Card::Output out;
  out.result.emplace().value = n * factor;
  tsugumi::visualization::text(std::to_string(n) + " × " + std::to_string(factor) + " = " + std::to_string(n * factor));
  return out;
}
```

### R

An R Card embeds R in the Card's worker. Messages are lists: the worker converts them in memory using the port’s FlatBuffers type, with no JSON files. Every invocation starts a new interpreter unless the Card turns `reuseWorker` on ([worker lifecycle](#r-and-julia-worker-lifecycle)).

`definitions/multiply/_card.json` (R)

```json
{
  "language": "r",
  "inputs": [{"name": "number"}],
  "params": [{"name": "factor", "type": "uint64", "data": 2}]
}
```

`definitions/multiply/default.nix` (R)

```nix
{ pkgs, inputs, params }: {
  run = inputs.tsugumi.rCard { src = ./src; };
}
```

`definitions/multiply/src/main.R` (R)

```r
main <- function(ctx) {
  n <- ctx$input("number")$value
  factor <- ctx$params$factor
  ctx$output("result", list(value = n * factor))
  ctx$display_text(sprintf("%d × %d = %d", n, factor, n * factor))
}
```

Packages come from nixpkgs' `rPackages` (`packages = ps: [ ps.survival ps.ggplot2 ];`). `ctx$plot(expr, alt = "description")` draws a figure in the Card's display (both base graphics and ggplot). Tables are `ctx$display_table(data.frame)`.

### Julia

A Julia Card embeds Julia in the Card's worker. Messages are Dicts, converted in memory using each port’s FlatBuffers type. Every invocation starts a new interpreter unless the Card turns `reuseWorker` on.

`definitions/multiply/_card.json` (Julia)

```json
{
  "language": "julia",
  "inputs": [{"name": "number"}],
  "params": [{"name": "factor", "type": "uint64", "data": 2}]
}
```

`definitions/multiply/default.nix` (Julia)

```nix
{ pkgs, inputs, params }: {
  run = inputs.tsugumi.juliaCard { src = ./src; };
}
```

`definitions/multiply/src/main.jl` (Julia)

```julia
function main(ctx)
    n = input(ctx, "number")["value"]
    factor = ctx.params["factor"]
    output(ctx, "result", Dict("value" => n * factor))
    display_text(ctx, "$n × $factor = $(n * factor)")
end
```

It runs with the standard library alone. The Nix helper retains `packages = [ "JuMP" ];`, but the pinned nixpkgs implementation uses import-from-derivation, which the Card evaluator disables; registry package resolution is currently rejected before the build. This limitation is independent of interpreter reuse. For a figure, pass a Vega-Lite spec as a Dict to `display_vegalite(ctx, spec)` and it is shown as an interactive chart.

### R and Julia worker lifecycle

**A Card keeps no state by default.** Every invocation gets a new worker and a new interpreter, which loads the source and then calls `main(ctx)`: nothing an earlier invocation or run did (globals, loaded packages, `options()`, the position of the random number generator) reaches the next one. Run it again by hand with the same inputs and it starts from the same place.

**To keep state, set `reuseWorker: true` in `_card.json`** ("Keep the worker's state" in the Card's settings). The Card then loads its source once and the same interpreter serves later invocations and runs. This is also what makes a Card that is called many times in a loop fast. What carries over from one call to the next:

- globals and module-level variables, and anything `library()` / `using` loaded or redefined;
- R's `options()`, `par()` and open graphics devices;
- the position of the random number generator (`.Random.seed`, Julia's default RNG): a second run continues the sequence where the first stopped unless the Card seeds it, in `main(ctx)` or in the hook below.

The working directory and environment variables do not carry over: the worker sets both again for every call. The optional `on_run_start(ctx)` hook runs once before the first invocation of each run; reset per-run state there. Each invocation's record says whether its worker had served an earlier call. The kept interpreter ends when the Card's source or packages change, after a failed, stopped or timed out call, and under the limits in [Resident workers](features.md#resident-workers); the next call then starts a new one.

Source loading happens before any invocation; read inputs and params inside `main(ctx)` or `on_run_start(ctx)`.

Every invocation receives a fresh context. Do not retain the context or use its functions from background threads: interpreter calls and host callbacks belong to the worker's main thread. `ctx$cancelled()` / `cancelled(ctx)` allow cooperative stopping; `ctx$remaining_ms()` / `remaining_ms(ctx)` return the remaining deadline in milliseconds, or `-1` without a deadline. A stop or expired deadline also terminates an uncooperative worker. Exceptions fail the invocation and appear in its log.

Displays publish immediately, including before a long calculation finishes. Each new display replaces the previous one. File and Directory inputs still use the worker's read-only paths, and outputs belong in the invocation's `output_directory`; those paths change on the next call. Nix continues to pin the interpreter and packages.

## The same Card in Ruby

Ruby uses MRI in the package worker. Inputs and parameters are Hashes and output is validated
against the FlatBuffers schema. The instance can retain state between invocations.

`definitions/multiply/_card.json` (Ruby)

```json
{"language":"ruby","inputs":[{"name":"number"}],"params":[{"name":"factor","type":"int64","data":2}]}
```

`definitions/multiply/default.nix` (Ruby)

```nix
{ pkgs, inputs, params }:
let sdk = inputs.tsugumi.sdk { languages = [ "ruby" ]; };
in { run = inputs.tsugumi.rubyCard { inherit sdk; src = ./src; }; }
```

`definitions/multiply/src/main.rb` (Ruby)

```ruby
def main(ctx)
  n = ctx.input('number').fetch('value')
  factor = ctx.params.fetch('factor')
  ctx.output('result', value: n * factor)
  ctx.display_text("#{n} × #{factor} = #{n * factor}")
end
```

For gems, keep a `Gemfile` and its `Gemfile.lock` together in the Card's directory. In the project terminal, run `bundle lock --add-platform ruby` from that directory. Add `lock = ./Gemfile.lock; hash = pkgs.lib.fakeHash;` to `rubyCard`. Run once and replace the placeholder with the fixed-output hash of the downloaded `.gem` files, reported as `got: sha256-…` in the Card's build log. The build then installs the locked gems offline. Native extensions can use libraries passed through `buildInputs`. When updating the Gemfile or lock, repeat the hash step.
Native gems are built by Nix; the worker never installs them at runtime. See the
[Ruby API](../agent/Reference/card-guide.md#ruby-srcmainrb) for files, displays and lifecycle details.

## What to read next

| To know | Document |
| --- | --- |
| Branches, merges and loops, passing files, data pinned with Nix | [The Card specification](../agent/Reference/card-guide.md), [The bundled demos](Demos.md) |
| Examples of code that runs | [demos/](../demos/) (`report`, `collatz` and others) |
| Leaving implementation to the Agent | [the Agent in Features](features.md#9-agent) |
| What the words mean | [Glossary](Glossary.md) |

## Go Cards


Use `"language": "go"` and start from the whole
`demos/en/go-pipeline/definitions/scale` folder. The **Go step** menu creates the
same scaffold. `default.nix` selects its typed SDK:

```nix
{ pkgs, inputs, params }:
let sdk = inputs.tsugumi.sdk { languages = [ "go" ]; };
in { run = inputs.tsugumi.goCard { inherit sdk; src = ./src; }; }
```

The source is one `package main` module. `go.mod` requires `tsugumi v0.0.0`
and replaces it with `./tsugumi`; Nix supplies that reserved directory and
`tsugumi_adapter.go`. Define both functions (the run hook may be a no-op):

```go
package main

import "tsugumi"

func run(ctx *tsugumi.Context, input tsugumi.Input) (tsugumi.Output, error) {
    params, err := ctx.Params()
    if err != nil { return tsugumi.Output{}, err }
    product := input.Number.Value * *params.Factor
    if err := ctx.DisplayScalar(float64(product), "product"); err != nil {
        return tsugumi.Output{}, err
    }
    return tsugumi.Output{Out: &tsugumi.OutputOut{Value: product}}, nil
}

func onRunStart(ctx *tsugumi.Context) error { return nil }
```

The generated `Input` fields are pointers to FlatBuffers object API values:
`input.Number.Value`, not a raw message buffer. Missing optional inputs are nil.
Every input/param buffer is copied to Go memory before unpacking, so values may
be retained safely. Outputs own objects; an alias `Output<Port>` (for example
`OutputOut`) identifies each output's type. A non-nil field emits that port;
required outputs are still checked by the host. Optional scalar params are
pointers, such as `params.Factor`. Use native `int64`/`uint64` without a float
conversion for full integer precision.

State in the library lives for the resident worker's lifetime. `onRunStart`
runs before the first invocation of each run; `reuseWorker: false` creates a
fresh process per invocation. The Go runtime is never unloaded from a live
worker. `Context` copies share an internal guard: after the invocation ends,
callbacks return `tsugumi.ErrClosed`. It exposes `RunID`, `Card`, `Invocation`,
`WorkingDirectory`, `TemporaryDirectory`, `OutputDirectory`, `Cancelled`,
`Done` and `Remaining` (`-1` means no deadline).

Use `ctx.Go(func() error { ... })` for concurrent work. Schedule tasks before
`run` returns; the SDK joins them before packing outputs and serializes host
callbacks. Task panics/errors fail the invocation. Select on `ctx.Done()` to
stop cooperatively. Cancellation or deadline also kills the entire worker,
including an uncooperative goroutine, and the next call starts clean. Ordinary
`go` statements are outside this task group: join them yourself, and do not
expect their panics to be recovered by the caller. Such a crash is still
reported as a worker failure. Data shared between tasks needs ordinary Go
synchronization.

File/Directory inputs have a `Value` path staged for this call. Write outputs
under `ctx.OutputDirectory()` and assign the path to the owned output's
`Value`. `ctx.Log` and `DisplayText`, `DisplayMarkdown`, `DisplayScalar`,
`DisplayLineChart` (one series), `DisplayTable`, `DisplayImage`, `DisplayPoints`
and `DisplayModule` return errors; propagate them. Displays stream while the
Card runs. Return `tsugumi.Fail("output_invalid", "explanation")` for a
structured failure, or an ordinary error for `worker_failed`.

The default scaffold uses the standard library and FlatBuffers' Go runtime
from the same pinned nixpkgs source as `flatc`. For external modules, keep
`go.mod` and `go.sum` in the Card's `src` and name the lock in `default.nix`:
`goCard { inherit sdk; src = ./src; lock = ./src/go.sum; hash = pkgs.lib.fakeHash; }`.
Run the Card once and copy the actual `got: sha256-…` value from the build log
into `hash`. Builds use a local immutable module cache and never update the
lock. A Card without `lock` and `hash` cannot fetch external modules.
Native libraries use `buildInputs`, and build tools use `nativeBuildInputs`.

To make or update the lock on request, use the project shell with Go on PATH:
`python3 <sdk>/go/update-lock.py <card>/src`. The helper runs `go mod tidy` in
a temporary copy and writes only `go.mod` and `go.sum` to the Card's source
directory. It accepts `--offline` for a populated module cache. Reload the
project after generating the files, then obtain the hash as above.

## C#

Choose Processing (C#) for a .NET Card. The [C# guide](CSharp.md) covers typed inputs, outputs and parameters, asynchronous work, NuGet dependencies and the cross-language demo.

## Rust Cards


Use `"language": "rust"`. Start from `demos/en/rust-pipeline/definitions/scale`
for a Card using only the standard library and SDK. The UI's **Rust step**
creates the same scaffold. `default.nix`:

```nix
{ pkgs, inputs, params }:
let sdk = inputs.tsugumi.sdk { languages = [ "rust" ]; };
in { run = inputs.tsugumi.rustCard { inherit sdk; src = ./src; }; }
```

`src/Cargo.toml` declares a `cdylib` and `tsugumi = { path = "tsugumi" }`.
That path is reserved for the crate generated from the Card's contract. The
SDK includes the Rust runtime source of the same FlatBuffers version as
`flatc`. A Card without a lock builds offline using the SDK's own crates.

To add crates.io dependencies on request, add them to `src/Cargo.toml`, then
use the project shell with Cargo on PATH. Build the FlatBuffers source from
the Card's nixpkgs pin and pass it to the lock helper. For a GitHub nixpkgs
pin, for example:

```sh
flatbuffers_src=$(nix build --no-link --print-out-paths "github:NixOS/nixpkgs/<revision>#flatbuffers.src")
python3 <sdk>/rust/update-lock.py <card>/src --flatbuffers-source "$flatbuffers_src"
```

The helper resolves in a temporary copy and writes only `src/Cargo.lock`.
Reload the project and name it in `rustCard` with
`lock = ./src/Cargo.lock; hash = pkgs.lib.fakeHash;`. Run once and copy the
`got: sha256-…` value from the build log into `hash`. This pins the whole
vendored directory. Repeat when updating dependencies. Native libraries go in
`buildInputs` and build tools such as pkg-config in `nativeBuildInputs`. The
Card never fetches crates when it runs.

```rust
use tsugumi::{Context, Input, Output, Result};

fn run(ctx: &Context<'_>, input: Input<'_>) -> Result<Output> {
    let mut out = tsugumi::outputs::Out::default();
    out.value = input.number.value() * ctx.params()?.factor().unwrap_or(1);
    ctx.display_scalar(out.value as f64, "product")?;
    Ok(Output { out: Some(out) })
}
tsugumi::export!(run);
```

`Input` contains verified, borrowed FlatBuffers views; optional ports are
`Option`. Inputs and `ctx.params()?` cannot outlive the current context. Copy or
`unpack()` a value before retaining it. `Output` holds owned FlatBuffers object
API types, aliased by port under `tsugumi::outputs` (for example `Out`). Construct
them with `Default` and assign fields: generated types are non-exhaustive.
Input/output namespaces differ, so explicitly construct an output object rather
than returning an input's owned object. `Some` emits a port; `None` omits it and
normal required-output validation still applies. Integers remain native `i64`/
`u64`, including values outside a double's exact range.

The worker retains the library and its state by default. Use
`tsugumi::export!(run, on_run_start)` with a `fn(&Context<'_>) -> Result<()>`
hook for per-run initialization; `reuseWorker: false` starts fresh each call.
Keep owned state in ordinary Rust synchronization/thread-local containers.
`Context` cannot be sent/shared across threads. `ctx.invocation()`, `run_id()`,
`card()`, `cancelled()` and `remaining_ms()` describe the current call.

File/Directory inputs are typed tables with a `value()` path, staged afresh for
each invocation. Write output files beneath `ctx.output_directory()` and put
that absolute path in the output object's `value`. Other paths are
`working_directory()` and `temporary_directory()`.

`ctx.log()` and all display methods return `Result<()>`: `display_text`,
`display_markdown`, `display_scalar`, `display_line_chart` (one series),
`display_table`, `display_image`, `display_points`, and `display_module` (Plotly/
Vega-Lite JSON). Displays stream before completion. Propagate errors with `?` or
return `Error::new("output_invalid", "explanation")`. Panics are caught at the
FFI boundary and fail the invocation; the failed worker is discarded. Explicit
abort, double panic during unwinding and native crashes still terminate the
worker, and the daemon records that failure. Cancellation/deadlines stop even
uncooperative native code; check the context for cooperative early exit.
