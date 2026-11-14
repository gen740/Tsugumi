# Your first Card

Make a Card that just multiplies the number it receives, run it, and try other conditions.
After making it in Python, replace the same Card with bash, C++, R and Julia.

This page is the Card specification and SDK reference. [C# Cards](CSharp.md), [visualisation templates](Visualization.md), [marimo notebooks](Marimo.md) and [R reports](Reports.md) describe their specialised APIs.

## What you will make

```text
start ──out→ settings ──number→ multiply
```

| Card | Kind | What it does |
| --- | --- | --- |
| `start` | Start (trigger) | Where the run begins |
| `settings` | Input value (input) | Sends a number you can edit on the Canvas |
| `multiply` | Python step | Multiplies the number it receives by `factor`, outputs the result and displays it |

When it is done, the Canvas looks like this. Below are the same Cards as on the real screen, and they run without installing anything. Press ▶ Run to watch the flow, and click `multiply` to read the Python code on this page (web version only).

<div data-player="card-guide"></div>

## Making it on screen

1. Make a project with "New Project…" on the home screen.
2. Right-click the Canvas and, from "Add Card", place "Start", "Input value" and "Python step", naming them `start`, `settings` and `multiply`.
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

From then on the Card is built from exactly those files, without the network. To change a dependency, edit `pyproject.toml`, repeat steps 2 to 4, and keep `pyproject.toml`, `uv.lock` and `default.nix` together in the Card's folder. A lock inconsistent with `pyproject.toml`, a missing hash or a mismatched hash fails the build.

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

The body is `main(inputs, report)`. Import inputs from `inputs.<port>.<namespace>.<table>` and outputs from `outputs.<port>.<namespace>.<table>`.

`definitions/multiply/src/main.py`

```python
import tsugumi
import outputs.result.hello.Result as result_out

def main(inputs, report):
    n = inputs.number.value
    factor = inputs.params.factor
    b = tsugumi.builder()
    result_out.Start(b)
    result_out.AddValue(b, n * factor)
    report.text(f"{n} × {factor} = {n * factor}")
    return {"result": tsugumi.message(b, result_out.End(b))}
```

## Running it and checking

1. Press "Run" (⌘R) at the top of the Canvas. Only the first time takes a while, as Nix prepares the environment.
2. `multiply` displays `21 × 2 = 42`.
3. "Run History" in the context menu shows the time and logs of each stage (Nix evaluation, build, run, saving).

## Trying other conditions

- **Change only a parameter.** Set `factor` of `multiply` to 3 and run only `multiply` with "Run Only This Card (Last Inputs)" in the context menu. `settings` does not run, and with the saved input the result is `21 × 3 = 63`. `factor` is not passed to Nix, so nothing is rebuilt either.
- **Change something upstream.** After changing `value` of `settings`, run again from `start`. Partial runs use saved inputs, so an upstream change does not arrive until you run the whole ([Rerunning from the middle](Rationale.md#rerunning-from-the-middle)).
- **Go back to an earlier version.** Each run commits the changes to git. From "Version History…" (⌥⌘Y) in the Workflow menu, go back to an earlier version and start a different attempt ([Versions](Versions.md)).

## Inputs, results and reports

A Card receives read-only `inputs`, returns its output ports, and publishes presentation through `report`. A Card that needs invocation metadata also asks for `run`. Parameters belong to `inputs.params`. Omit unused trailing roles; requesting `run` requires the preceding `report` argument.

| Language | Entry point and result |
| --- | --- |
| Python | `main(inputs[, report[, run]])` returns a mapping by port, or a generator yielding mappings |
| C++ | `auto run(const Card::Inputs& inputs[, tsugumi::Report& report[, const tsugumi::Run& run]]) -> Card::Output` |
| R | `main <- function(inputs, report, run)` returns a named list; omit unused trailing arguments |
| Julia | `main(inputs, report, run)` returns a `Dict`; omit unused trailing arguments |
| Go | `func run(inputs tsugumi.Inputs, report *tsugumi.Report, run *tsugumi.Run) (tsugumi.Output, error)`; omit unused trailing arguments |
| Rust | `fn run(inputs: Inputs<'_>, report: &Report<'_>, run: &Run<'_>) -> Result<Output>` |
| Ruby | `def main(inputs, report, run)` returns a Hash or Enumerator; omit unused trailing arguments |
| C# | `ValueTask<Outputs> RunAsync(Inputs inputs, Report report, Run run)`; omit unused trailing arguments |
| Bash | Input files `$JSON_<PORT>`, paths `$FILE_<PORT>` / `$DIR_<PORT>` and `$TSUGUMI_PARAMS_JSON`; result JSON to `$TSUGUMI_OUTPUT`; presentation JSON to `$TSUGUMI_REPORT` |

In C++ parameters are `inputs.params`; Go uses `inputs.Params()` and port accessors such as `inputs.Number()`; Rust uses `inputs.params`; C# uses `inputs.Params`. Python has `inputs.number.value` and `inputs.params.threshold`, with generated lowerCamelCase field names. `inputs.input(port, GeneratedTable)` and `inputs.input_bytes(port)` also offer the low-level FlatBuffers view and bytes. R uses `inputs$number$value`, Julia `inputs.number["value"]`, Ruby `inputs.number.fetch('value')`. Optional ports can be absent.

Inputs do not expose emission or presentation callbacks. C++ takes a const object and Rust borrows verified table views. Python freezes object attributes and arrays, Ruby recursively freezes values, and R rejects assignment through its input facade. Go accessors unpack fresh owned copies, C# properties unpack fresh managed copies, and Julia reads copies: editing one of these detached messages cannot modify the inputs received by the step.

### Building results

Python returns generated output `TableT` objects directly: `return {"result": result}`. A finished builder or bytes can also be a mapping value. `tsugumi.builder()` constructs a FlatBuffers builder; `tsugumi.message(builder, root)` finishes it and owns its bytes. `tsugumi.message(builder)` copies a builder already finished by the Card. This also allows building several roots in one builder, snapshotting each result before reusing the builder.

C++, Go and Rust keep their generated owned `Output` object. Set an optional field for each produced port and return it. The adapter packs it after the function succeeds (and after Go's managed tasks finish). Each port may be produced once per invocation. The low-level C ABI is an adapter implementation detail.

R lists, Julia Dicts and Ruby Hashes carry messages checked by the JSON face against each port's FlatBuffers schema. C# returns generated `Outputs` properties, or uses its byte dictionary for finished buffers: `new Outputs { ["result"] = builder.SizedByteArray() }`. The adapter packs each typed property after awaiting the function.

File and Directory outputs use the same result forms as other messages. Their `value` / `Value` field is an absolute path beneath `run.output_directory` (Go `run.OutputDirectory()`, C# `run.OutputDirectory`, Bash `$TSUGUMI_OUTPUT_DIR`). Write the files there, then return the path message. A Directory result includes its files and nested directories; [Run history](RunHistory.md#keep-and-open-artifacts) describes retention. The run object also exposes working and temporary directories, run and Card IDs, invocation number, cancellation and remaining time.

### Results during a long invocation

Python generators yield mappings incrementally; a final returned mapping is emitted too. Each port may be produced once per invocation; a repeated port in a later batch fails with `output_invalid`. Use `yield from helper(...)` when a helper returns a stream. Ruby returns an Enumerator which yields Hashes. Julia returns an iterator of Dicts (a Channel can produce them incrementally). R returns a zero-argument closure yielding the next named list, then `NULL` at end. C# returns `Outputs.Stream(IAsyncEnumerable<Outputs>)`; the adapter awaits and emits each batch before advancing the iterator. These streams let those SDKs build successive port batches without putting a write method on any role object. The worker buffers port results until the invocation completes; an error after an earlier batch still fails the invocation. Report displays publish live.

### Presentation and hooks

Report methods are `text`, `markdown`, `scalar`, `line_chart`, `table`, `image`, `points`, `figure`, `module` and `log`, where supported by that language. Go and C# use PascalCase. Bash uses `$TSUGUMI_REPORT` for presentation JSON and captured stdout/stderr for diagnostics. Julia uses functions such as `markdown(report, text)`; its module method is `report.module(name, config)` because `module` is a keyword. Presentation publishes immediately and a new display replaces the preceding display; logs remain log lines.

Python uses `report.figure(fig)` for Matplotlib, Plotly or Altair. `report.figure()` renders open Matplotlib figures, drives animations until they finish or the run stops, then closes them. `report.figure(wait=seconds)` publishes live frames and drives timers for that interval. Drawing, `plt.show()` and leaving a figure open do not publish on their own; invocation cleanup closes open figures. R uses `report$figure(expr)` for base graphics or ggplot. Third-party libraries can still write their own diagnostic stdout/stderr; Card-authored presentation and log lines should use Report.

`on_run_start`, C++/Go `onRunStart` and C# `OnRunStartAsync` receive the same three roles as the main function, using the first invocation's inputs and params. They run before the first invocation of each run. Python, R, Julia, Ruby and C# hooks may return results using their main-function result form; typed C++/Go/Rust hooks remain initialization-only. No role is supplied at source-loading time. A resident worker keeps language state only when `reuseWorker` is enabled.

Rust selects arity explicitly: `export!(run, inputs)` for inputs only, `export!(run)` for inputs and report, `export!(run, with_run)` for all three. Add the hook as the last macro argument; the hook receives all three roles and returns `Result<()>`.

Bash `preRun` / `postRun` receive no Card inputs; see [Worker state](#worker-state) for their lifecycle.

## Writing the same Card in another language

As long as the input and output types are the same, what is inside can be replaced while the wires on the Canvas and the Cards around it stay as they are.

### Python, with plain objects

The generated Python classes also come in an object form (`NumberT`, `ResultT`, `ParamsT`): plain attributes named after the schema's fields (`photon_count` becomes `photonCount`), and `Pack` to build an output. `tsugumi.contract` gives an input or the params in that form without importing the class by name, and lists the Card's ports (`contract.ports()`, `contract.port("number")["kind"]`). It is meant for code shared by many Cards, which cannot know the port types when it is written.

`definitions/multiply/src/main.py` (Python objects)

```python
from outputs.result.hello.Result import ResultT

def main(inputs, report):
    n = inputs.number.value
    factor = inputs.params.factor
    result = ResultT()
    result.value = n * factor
    report.text(f"{n} × {factor} = {n * factor}")
    return {"result": result}
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

For a display, write JSON to `$TSUGUMI_REPORT`: one of `{"markdown": "**bold**"}`, `text`, `table` (`columns` and `rows`), `image` (`path` and `alt`) or `module` (`module`, `config`, and optional `blobs`, a map of names to local file paths). It is shown when the script ends normally.

To log and show a value of any connected type, see the ordinary Bash Card in
[`demos/en/marimo/definitions/result`](../demos/en/marimo/definitions/result).
It declares a required `in` port and lets the connection supply its type.
In `text` mode, the worker's formatted value goes to stdout and to the
Canvas as text. The format can include `nan` and `inf`, so it is meant for
inspection and does not promise strict JSON. Byte vectors appear as arrays of
numbers. File and Directory messages show their staged paths.

Set `format` to `markdown` to render a string field (named `value` by default).
Set `markdown_field` to select another top-level field. This mode reads the
input as an object and fails if the selected field is missing or is not a
string. The full formatted message still goes to stdout. Canvas displays
larger than 1 MiB are shortened with a marker, independently of the normal
worker and log limits. Each run logs and displays the value again, even when
the Card's build is reused.

### C++

The input and output ports become members of `Card::Inputs` and `Card::Output` as they are.

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

auto run(const Card::Inputs& inputs, tsugumi::Report& report) -> Card::Output {
  const auto n = inputs.number.value;
  const auto factor = inputs.params.factor.value_or(1);
  Card::Output out;
  out.result.emplace().value = n * factor;
  report.text(std::to_string(n) + " × " + std::to_string(factor) + " = " + std::to_string(n * factor));
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
main <- function(inputs, report) {
  n <- inputs$number$value
  factor <- inputs$params$factor
  report$text(sprintf("%g × %g = %g", n, factor, n * factor))
  list(result = list(value = n * factor))
}
```

Packages come from nixpkgs' `rPackages` (`packages = ps: [ ps.survival ps.ggplot2 ];`). `report$figure(expr, alt = "description")` draws a figure in the Card's display (both base graphics and ggplot). Tables are `report$table(data.frame)`.

Use `inputs$input("events")` for a message and `inputs$path("events")` for a File/Directory path. Wrap array fields in `I()` even when they contain one element: `list(counts = list(bins = I(c(1, 2))))`. Integers beyond R's exact double range, ±(2^53 − 1), arrive as decimal strings, preserving signed and unsigned 64-bit values. Return an R integer (`27L`) or a decimal string for a `long`/`ulong` field; `27` is a double and serializes as `27.0`, which that type rejects. `stop("why")` fails the invocation with its diagnostic.


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
function main(inputs, report)
    n = inputs.number["value"]
    factor = inputs.params["factor"]
    report.text("$n × $factor = $(n * factor)")
    return Dict("result" => Dict("value" => n * factor))
end
```

It runs with the standard library alone. The Nix helper retains `packages = [ "JuMP" ];`, but the pinned nixpkgs implementation uses import-from-derivation, which the Card evaluator disables; registry package resolution is currently rejected before the build. This limitation is independent of interpreter reuse. For a figure, pass a Vega-Lite spec as a Dict to `vegalite(report, spec)` and it is shown as an interactive chart.

### R and Julia worker lifecycle

The [worker state rules](#worker-state) apply to both interpreters. With `reuseWorker: true`, globals, loaded modules and RNG state survive invocations and runs. R also retains `options()`, `par()` and open graphics devices. Set the random seed explicitly when reproducibility requires it; reset per-run state in the run-start hook.

Read inputs and params inside the entry point or hook. Each invocation receives fresh Inputs, Report and (when requested) Run objects; copied input messages may be retained. Report/Run callbacks belong to the interpreter's invocation thread and must not be retained or used in background work. `run$cancelled()` / `cancelled(run)` support cooperative stopping, and `run$remaining_ms()` / `remaining_ms(run)` return milliseconds remaining, or `-1` without a deadline. A stop/deadline also terminates uncooperative code. Displays publish immediately and each new display replaces the previous one. File/Directory paths change on the next invocation.

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
def main(inputs, report)
  n = inputs.number.fetch('value')
  factor = inputs.params.fetch('factor')
  report.text("#{n} × #{factor} = #{n * factor}")
  { result: { value: n * factor } }
end
```

See the [Ruby API](#ruby-api) for files, reports, lifecycle and dependency locks.

## Worker state

Cards start a fresh worker for each invocation by default. Set `reuseWorker: true` to retain state across invocations and runs, and use the [run-start hook](#presentation-and-hooks) to reset per-run state. Each invocation records whether its worker was reused. Workers end when their build conditions change, on failure, cancellation or deadline, or when the project closes. Idle workers end after ten minutes; resident workers are limited to 2 GiB and 32 workers (least recently used ends first).

Source loading precedes any invocation and any `preRun`. The working directory and environment are set again on each call; `preRun`'s exports and `cd` reach the entry point, including Python's `os.environ`, but cannot affect libraries already loaded. Configure native libraries at the top of `main.py`, before importing them:

```python
import os
os.environ["OMP_NUM_THREADS"] = "1"
import numpy as np
```

Top-level settings last for the worker; environment changes made inside an entry point or hook last for that call. `HOME`, `XDG_CONFIG_HOME` and `XDG_CACHE_HOME` are writable worker directories for library caches.

The shell steps are `preRun → run → postRun`. `preRun` and `postRun` receive no Card inputs. `postRun` also runs after a failed earlier step, with `TSUGUMI_FAILED=1` and the produced port names in `TSUGUMI_OUTPUTS`; it cannot clear the failure. Cancellation or a deadline prevents further steps, including `postRun`, from starting. File/Directory results are written under the invocation's output directory, then returned as path messages; see [Building results](#building-results).

## What to read next

| To know | Document |
| --- | --- |
| Branches, merges and loops, passing files, data pinned with Nix | [The Card specification](CardGuide.md), [The bundled demos](Demos.md) |
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

func run(inputs tsugumi.Inputs, report *tsugumi.Report) (tsugumi.Output, error) {
    params := inputs.Params()
    product := inputs.Number().Value * *params.Factor
    if err := report.Scalar(float64(product), "product"); err != nil {
        return tsugumi.Output{}, err
    }
    return tsugumi.Output{Out: &tsugumi.OutputOut{Value: product}}, nil
}

func onRunStart(inputs tsugumi.Inputs, report *tsugumi.Report) error { return nil }
```

The generated `Inputs` accessors return owned snapshots to FlatBuffers object API values:
`inputs.Number().Value`, not a raw message buffer. Missing optional inputs are nil.
Every input/param buffer is copied to Go memory. Accessors unpack fresh snapshots,
which may be retained and edited without changing the received inputs. Outputs own objects; an alias `Output<Port>` (for example
`OutputOut`) identifies each output's type. A non-nil field emits that port;
required outputs are still checked by the host. Optional scalar params are
pointers, such as `params.Factor`. Use native `int64`/`uint64` without a float
conversion for full integer precision.

State in the library lives for the resident worker's lifetime. `onRunStart`
runs before the first invocation of each run; `reuseWorker: false` creates a
fresh process per invocation. The Go runtime is never unloaded from a live
worker. `Report` and `Run` copies share an internal guard: after the invocation ends,
callbacks return `tsugumi.ErrClosed`. It exposes `RunID`, `Card`, `Invocation`,
`WorkingDirectory`, `TemporaryDirectory`, `OutputDirectory`, `Cancelled`,
`Done` and `Remaining` (`-1` means no deadline).

Use `run.Go(func() error { ... })` for concurrent work. Schedule tasks before
`run` returns; the SDK joins them before packing outputs and serializes host
callbacks. Task panics/errors fail the invocation. Select on `run.Done()` to
stop cooperatively. Cancellation or deadline also kills the entire worker,
including an uncooperative goroutine, and the next call starts clean. Ordinary
`go` statements are outside this task group: join them yourself, and do not
expect their panics to be recovered by the caller. Such a crash is still
reported as a worker failure. Data shared between tasks needs ordinary Go
synchronization.

File/Directory inputs have a `Value` path staged for this call. Write outputs
under `run.OutputDirectory()` and assign the path to the owned output's
`Value`. `report.Log` and `Text`, `Markdown`, `Scalar`,
`LineChart` (one series), `Table`, `Image`, `Points`
and `Module` return errors; propagate them. Displays stream while the
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

To make or update the lock on request, use the human Terminal panel with Go on PATH:
`python3 <sdk>/go/update-lock.py <card>/src`. The helper runs `go mod tidy` in
a temporary copy and writes only `go.mod` and `go.sum` to the Card's source
directory. It accepts `--offline` for a populated module cache. Reload the
project after generating the files, then obtain the hash as above. The Agent
uses `project_save` to write source and lock files; its shell is read-only.

## C#

Choose C# Card for a .NET Card. The [C# guide](CSharp.md) covers typed inputs, outputs and parameters, asynchronous work, NuGet dependencies and the cross-language demo.

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
use the human Terminal panel with Cargo on PATH. Build the FlatBuffers source from
the Card's nixpkgs pin and pass it to the lock helper. For a GitHub nixpkgs
pin, for example:

```sh
flatbuffers_src=$(nix build --no-link --print-out-paths "github:NixOS/nixpkgs/<revision>#flatbuffers.src")
python3 <sdk>/rust/update-lock.py <card>/src --flatbuffers-source "$flatbuffers_src"
```

The helper resolves in a temporary copy and writes only `src/Cargo.lock`.
The Agent writes source and lock files with `project_save`; its shell is read-only.
Reload the project and name it in `rustCard` with
`lock = ./src/Cargo.lock; hash = pkgs.lib.fakeHash;`. Run once and copy the
`got: sha256-…` value from the build log into `hash`. This pins the whole
vendored directory. Repeat when updating dependencies. Native libraries go in
`buildInputs` and build tools such as pkg-config in `nativeBuildInputs`. The
Card never fetches crates when it runs.

```rust
use tsugumi::{Report, Run, Inputs, Output, Result};

fn run(inputs: Inputs<'_>, report: &Report<'_>) -> Result<Output> {
    let mut out = tsugumi::outputs::Out::default();
    out.value = inputs.number.value() * inputs.params.factor().unwrap_or(1);
    report.scalar(out.value as f64, "product")?;
    Ok(Output { out: Some(out) })
}
tsugumi::export!(run);
```

`Inputs` contains verified, borrowed FlatBuffers views; optional ports are
`Option`. Inputs and `inputs.params` cannot outlive the current call. Copy or
`unpack()` a value before retaining it. `Output` holds owned FlatBuffers object
API types, aliased by port under `tsugumi::outputs` (for example `Out`). Construct
them with `Default` and assign fields: generated types are non-exhaustive.
Input/output namespaces differ, so explicitly construct an output object rather
than returning an input's owned object. `Some` emits a port; `None` omits it and
normal required-output validation still applies. Integers remain native `i64`/
`u64`, including values outside a double's exact range.

With `reuseWorker: true`, the worker retains the library and its state. Use
`tsugumi::export!(run, on_run_start)` with a `fn(Inputs<'_>, &Report<'_>, &Run<'_>) -> Result<()>`
hook for per-run initialization; `reuseWorker: false` starts fresh each call.
Keep owned state in ordinary Rust synchronization/thread-local containers.
`Report` and `Run` cannot be sent/shared across threads. `run.invocation()`, `run_id()`,
`card()`, `cancelled()` and `remaining_ms()` describe the current call.

File/Directory inputs are typed tables with a `value()` path, staged afresh for
each invocation. Write output files beneath `run.output_directory()` and put
that absolute path in the output object's `value`. Other paths are
`working_directory()` and `temporary_directory()`.

`report.log()` and all display methods return `Result<()>`: `text`,
`markdown`, `scalar`, `line_chart` (one series),
`table`, `image`, `points`, and `module` (Plotly/
Vega-Lite JSON). Displays stream before completion. Propagate errors with `?` or
return `Error::new("output_invalid", "explanation")`. Panics are caught at the
FFI boundary and fail the invocation; the failed worker is discarded. Explicit
abort, double panic during unwinding and native crashes still terminate the
worker, and the daemon records that failure. Cancellation/deadlines stop even
uncooperative native code; check `run.cancelled()` for cooperative early exit.


## Card-owned web views

A view is fixed by the Card's Nix definition. Write it either as a JavaScript
ES module using DOM, canvas, WebGPU or bundled libraries, or as C/C++ compiled
to a small Wasm module using standard `webgpu.h` and the SDK's shared host.
Choose JavaScript for DOM/SVG, plotting libraries and direct browser APIs;
choose C/C++ for existing native rendering code or portable GPU rendering code. No Tsugumi drawing API is
involved. Pointer handling stays in the frame and works
after the worker exits. The built-in line chart uses this same module mechanism
on both platforms; Card authors keep their existing line-chart SDK calls.

Declare the view alongside the implementation in `default.nix`:

```nix
{ pkgs, inputs, params }:
let
  sdk = inputs.tsugumi.sdk { languages = [ "python" ]; };
in {
  views.points = inputs.tsugumi.viewModule {
    src = ./view;
    entry = "index.js";  # default
    name = "point-cloud-view";
  };
  run = inputs.tsugumi.pythonCard { inherit sdk; src = ./src; };
}
```

`viewModule` uses esbuild to bundle local imports into `$out/index.js`, a
self-contained ES module. It makes no runtime downloads. Bundle dependencies
in the source directory or pass an already prepared source derivation. Import
`.wasm` as bytes (`import wasm from './solver.wasm'`, then
`WebAssembly.instantiate(wasm, imports)`); PNG, JPEG and SVG imports become data
URLs. Do not use runtime relative fetches or `import.meta.url` to locate files:
the UI imports the built entry from a blob URL, with network access disabled.
All assets must be incorporated at build time. This output directory is shared
and cached by Nix, referenced by the Card manifest's closure and fixed with the
Card's SDK. It is not a per-run artifact.

The entry must export these functions (each may return a Promise):

```javascript
export function initialise(root, { config, blobs }, size) {
  // root is a plain HTMLElement; blobs is { name: Uint8Array }.
  // size is { width, height, pixelRatio, visible, colorScheme }, in CSS pixels.
}
export function update({ config, blobs }) {}
export function resize({ width, height, pixelRatio, visible, colorScheme }) {}
export function dispose() {}
```

`initialise` runs once per loaded identity, `update` replaces all input data,
`resize` supplies geometry and visibility, and `dispose` releases timers,
listeners, GPU resources and library instances. Treat a display as a full
replacement snapshot. Export `snapshot()` returning a PNG data URL for DOM
views or libraries with their own image exporter. Otherwise previews capture
the first canvas. A WebGPU view should redraw before its snapshot and await
`device.queue.onSubmittedWorkDone()`. The canvas remains a cached still image;
Expand creates an interactive frame. Closing it releases the frame.

In Python, send computed bytes separately from the small JSON configuration:

```python
report.module("points", {"count": count}, {"points": packed_xyz})
```

Values support the contiguous Python buffer protocol; the native bridge holds
the buffer only during the call. C++ accepts `std::vector<tsugumi_blob>` as the
third argument of `report.module`. The ABI is version 5;
`tsugumi_blob` contains `name`, `data` and `size`. A display on the RPC carries
`Module { module, config_json, blobs }`, where `module` is the immutable
Nix store directory, not the declaration's short name. Configuration is limited
to 1 MiB; up to 64 blobs together to 8 MiB (within the existing 16 MiB frame limit). Plotly and Vega-Lite are view
outputs of this SDK and use the same loader and RPC.

The [view isolation boundary](security.md#showing-card-views-line-charts-plotly-vega-lite-and-card-owned-modules) applies to both platforms; code is separate from retained display data.

The runnable example is
[`tests/fixtures/view-point-cloud`](../tests/fixtures/view-point-cloud): Python
computes 12,000 little-endian float32 XYZ triples, and the JavaScript view
rotates them on drag through WebGPU alone. It gives a clear error if WebGPU or
an adapter is unavailable.

### C/C++ views with standard WebGPU

Declare a Wasm view in the same `views` attribute set:

```nix
views.points = inputs.tsugumi.wasmViewModule {
  src = ./view;
  sources = [ "main.c" ];  # default; .cc/.cpp sources use C++20
  name = "point-cloud-wasm-view";
  # cflags = [ "-DMY_OPTION=1" ];
};
```

The processing Card and `report.module("points", config, blobs)` are unchanged.
The builder compiles C11 or C++20 with Emscripten as a Wasm side module. Its
output contains `view.wasm`, a byte-count file and an `index.js` symlink to
`inputs.tsugumi.wasmViewTemplate`, one shared Nix output independent of every
Card's source. `inputs.tsugumi.wasmViewHeaders` supplies the headers separately.
The standard `<webgpu/webgpu.h>` is the webgpu-headers snapshot from the pinned
nixpkgs `wgpu-native.src` (no Rust library build). The small portable header is
[`<tsugumi/view.h>`](../sdk/include/tsugumi/view.h):

| Host function | Meaning |
| --- | --- |
| `tsugumi_view_config(size_t*)` | Borrowed JSON bytes; length excludes the trailing NUL |
| `tsugumi_view_blob(const char*, size_t*)` | Borrowed named binary input; missing is NULL/0 |
| `tsugumi_view_size(TsugumiViewSize*)` | Width/height in CSS pixels and pixel ratio |
| `tsugumi_view_on_pointer(callback)` | Down/move/up/cancel, local CSS coordinates and button mask |
| `tsugumi_view_on_wheel(callback)` | Wheel deltas normalized to CSS pixels |
| `tsugumi_view_on_frame(callback)` | Animation callback in monotonic milliseconds, paused while hidden |
| `tsugumi_view_error(const char*)` | Stop and report a UTF-8 diagnostic |

Callbacks are replaced by registration; NULL unregisters. Input pointers remain
valid until the next update or disposal. Updating replaces all blobs, including
removing absent names. Inputs are copied into Wasm memory, never stored as build
outputs. Pointer capture and browser event normalization belong to the shared
host; neither DOM objects nor JavaScript types appear in the C header.

Implement these four C-linkage entry points (no `main`):

```c
#include <tsugumi/view.h>
void tsugumi_view_initialise(WGPUDevice device, WGPUSurface surface, WGPUTextureFormat format);
void tsugumi_view_update(void);
void tsugumi_view_resize(void);
void tsugumi_view_dispose(void);
```

The host passes borrowed standard GPU handles, configures the surface at the
current pixel size and owns its presentation, device and surface lifetime.
Create pipelines/buffers with ordinary `wgpu*` calls, get the current surface
texture to render, and release each acquired handle. Do not reconfigure or
release the borrowed device/surface, or call `wgpuSurfacePresent` (Emdawnwebgpu
does not implement that blocking call). Register a frame callback to redraw;
snapshots invoke it and wait for the GPU queue before exporting PNG. Dispose
unregisters callbacks and releases the view's buffers, pipelines and other GPU
resources. The host cancels animation, removes listeners and frees input memory.
Initialization failures, callback failures and device loss reach the ordinary
module error bridge. No worker needs to remain alive.

Use the [C/Wasm point-cloud example](../tests/fixtures/view-point-cloud-wasm) to try the API.

### Built-in line chart

`report.line_chart` calls (and their other-language
equivalents) produce the built-in `line-chart` view. The JSON configuration is `{series: [{id, label, offset, count}, ...]}`.
Offsets and counts index two named blobs, `x` and `y`, each a concatenation of
little-endian float64 arrays. The limit is 10,000 points and
1 MiB of coordinate/label data. The view subtracts the visible origin in double
precision before converting to float32 GPU vertices, preserving small differences
in large values such as timestamps. Multiple series, the four-colour palette,
legends, live updates and canvas previews use the shared module lifecycle.
`size.colorScheme` is `light` or `dark`, supplied by the UI; appearance is part
of preview identity. Coordinates and display retention are independent of theme.


## Card definition reference

### A Card's folder

`definitions/<id>/` belongs to one Card (its id is its name):

| File | What it is |
| --- | --- |
| `_card.json` | All settings: `language` or `template`, `inputs`, `params`, run settings |
| `_meta.json` | The intent, for every kind of Card: `description` (its summary, shown on the Canvas and in Card settings; `_card.json` has none), `content`, `context`, port descriptions. When implementing a Draft, preserve its intent and fill an empty `description` and `context` from the user and Draft |
| `_outputs.fbs` | One table per output port, root table `Outputs` |
| `default.nix` | Code Cards only: a function `{ pkgs, inputs, params }:` returning `{ run = ...; }` with one implementation or an ordered list |
| `src/` | Code Cards only: the sources |

Never write `_inputs.fbs`: input types come from the connected outputs.

### Built-in Cards (`"template"` in `_card.json`, no code)

| Template | Ports | Use |
| --- | --- | --- |
| `trigger` | out `out` | Where a whole run starts |
| `input` | in `start` (optional), outs from `_outputs.fbs` | Hands on values or a project file (below) |
| `fork` | in `in`, outs `out1`, `out2` (param `branches`: string[] for other names) | Sends one value to several Cards (an output has one wire: this is how a value reaches several inputs) |
| `branch` | ins `value`, `condition` (a table with `value:bool`), outs `a` (true), `b` (false) | Sends to one side |
| `merge` | ins as declared in `inputs`, out `out` (a union of them) | Joins paths of a loop |
| `collect` | in `in`, out `out` (derived from the input) | Sends a batch as one typed vector or Directory |

To show a received value without choosing its type in advance, use an ordinary
Bash Card with an untyped input. The marimo demo's `result` Card logs
`$JSON_IN` to stdout and sends it as text to `$TSUGUMI_REPORT`. For Markdown,
select the string field to display explicitly.

### An input Card with fixed values

`demos/en/report/definitions/settings`: params named like the fields of its output table.

```json
{"template": "input", "params": [{"name": "seed", "type": "uint64", "data": 42}]}
```

### An input Card that hands on a project file

The file stays in the project; at run time it is copied into the run and the
next Card receives its path.

`definitions/load/_card.json` (the path is relative to this Card's definition folder):

```json
{"template": "input",
 "params": [{"name": "value", "type": "string", "data": "data/events.fits"}]}
```

Use `project:definitions/shared/data/events.fits` for a saved source in another
Card of the owning project.
The same rules apply inside external projects: `project:` names that external
project's root. Paths cannot be absolute or contain `..`, and must name a saved
project source. The base never changes according to which file exists.

`_outputs.fbs` (a File port: one table whose only field is `value:string`):

```
attribute "tsugumi";
namespace data;
table EventsFile { value:string; }
table Outputs { events:EventsFile (required, tsugumi: "file"); }
root_type Outputs;
```

The next Card reads it as a path: `inputs.input("events", EventsFile).Value().decode()` (Python),
`$FILE_EVENTS` (bash).


### Code Card settings

Set `language` to `python`, `cpp`, `go`, `rust`, `csharp`, `bash`, `r`, `julia` or `ruby`; remove `template` for a Code Card. Declare input port names in `inputs`; connected outputs determine their types. Add `filesystem: true` for File/Directory outputs. Parameters are entries such as `{"name": "low", "type": "double", "data": 600}`. Their types are `bool`, `int64`, `uint64`, `double`, `string` and arrays of those types; they are part of `inputs.params` at runtime. A parameter with `nix: true` is also a build-time value in `default.nix`.

Network access is disabled unless `_card.json` explicitly sets `network: true` (Experimental). TLS uses `SSL_CERT_FILE`; report downloaded data's URL, size and SHA-256. For immutable public data, prefer a pinned Nix fetch as described below.

Omit `executionService` for local execution, or select a host-registered service ID. `executionResources: {"cpuMillis": 1000, "memoryBytes": 4294967296}` caps CPU and memory when the service advertises limits, including its build. Connection details and credentials belong to the host; see [Remote execution](RemoteExecution.md). Worker state and run hooks follow the [worker state rules](#worker-state).

### Run lists and connected Nix exports

Use `demos/en/signal-sweep` for a parameter sweep. The `samples` Card uses a Nix
function to make three Bash implementations and declares shared
`exports.thresholds = [ 10 14 ];`. The `score` Card receives `samples.out` on its
`signal` port. Its `default.nix` reads `inputs.signal.thresholds` and uses
`run = map compare thresholds;` to make two implementations. Each implementation
reads the runtime signal from `$JSON_SIGNAL`. Nix exports hold build-time values,
functions or derivations. The port message carries the runtime value.

The three input items and two implementations produce six invocations, in input
item order with implementation order varying fastest. Every batch input is an
independent axis. `maxRuns` counts triggers, and a failed member cancels the
rest of the batch. Inspect each member's history for its inputs and result.
An empty list yields an empty batch.

Add a built-in `collect` after `score` to send one ordinary port value with an
`items` vector. The next Bash Card can read `.items` from its `$JSON_RESULTS` input file, as
`summary/default.nix` does. Collect turns File or Directory batches into a
Directory. The ordered `items` records contain each original name and a path
relative to the Directory root.

Refer to Nix exports as `inputs.<destination-port>`. Fork, Collect, Branch's
`value` input and Workflow boundaries preserve a unique Code producer. Use a
direct connection to that Code Card if the producer would otherwise be
ambiguous. The `score` Card also exports `thresholdCount`, which `summary` reads through
Collect as `inputs.results.thresholdCount`. Each run implementation shares
the same exports.


### Data fixed by Nix

Large public data (an observation, a reference genome, a simulation's result) can be fixed like a
package: fetched once into the Nix store by its hash, never during a run. A param with `"nix":
true` is readable in `default.nix` as `params.<name>` (changing it rebuilds the Card):

```nix
{ pkgs, inputs, params }:
let
  raw = pkgs.fetchurl {
    url = "ftp://host/path/${params.file}";  # explicit FTPS: ftp:// with
    curlOptsList = [ "--ssl-reqd" ];           # --ssl-reqd (often faster than https)
    hash = params.hash;                        # "sha256-..." (nix hash file <file>)
  };
  data = pkgs.runCommand "data.fits" { } "gunzip -c ${raw} > $out";
in
{
  run = ''
    printf '{"events": {"path": "%s"}}' "${data}" > "$TSUGUMI_OUTPUT"
  '';
}
```

The store path goes on as a plain string (a FlatBuffer table `{ path:string; }`, not a File port,
whose files must be in the Card's output folder); every Card can read `/nix/store`. A long build
step (a simulation) is fixed the same way with `pkgs.runCommand` or `stdenv.mkDerivation`.

### File and Directory outputs

`demos/en/report/definitions/generate` (File) and `demos/en/report/definitions/render` (Directory):
`attribute "tsugumi";`, the port marked `(required, tsugumi: "file")` or `(required, tsugumi:
"directory")`, a table with `value:string`, `"filesystem": true`, and the files written under
`run.output_directory` (Python) / `$TSUGUMI_OUTPUT_DIR` (bash).

### Reading a merge output

The input is a table whose `value` is a union with one member per merge input. C++:
`Card::visit(inputs.arrival.value, [](const auto& v) { return v.value; })` calls the lambda
with whichever member arrived (`inputs.arrival.value.type` says which port). Python:
`demos/en/collatz/externals/collatz-py/definitions/check/src/main.py`.


### Ruby API

`_card.json`: `{"language": "ruby", ...}`. Use the typed SDK and MRI package:

```nix
{ pkgs, inputs, params }:
let sdk = inputs.tsugumi.sdk { languages = [ "ruby" ]; };
in { run = inputs.tsugumi.rubyCard { inherit sdk; src = ./src; }; }
```

```ruby
def main(inputs, report, run)
  outputs = {}
  value = inputs.input('number').fetch('value') * inputs.params.fetch('factor')
  outputs['result'] = {value: value}
  report.text("result: #{value}")

  outputs
end
```

Inputs and params are owned Hashes / Arrays; output takes a Hash or a value object with `to_h`.
Field names may be strings or symbols. Integers retain all 64 bits and out-of-range values fail;
strings must be UTF-8 text. Use `Tsugumi.bytes(binary_string)` for a byte vector. A missing optional
input is `nil`; a message field may be `nil`, but a returned nil output message is invalid. Omit the
port to leave an optional output unproduced. FlatBuffers checks the port schema.

`inputs.path('file')` reads a File / Directory input path. Write artifacts under `run.output_directory`
and emit `{value: path}`. `run.temporary_directory`, `run.run_id`, `run.card`, `run.invocation` and
`run.working_directory` are also available. `report.log(text)` writes a log line. Interpreter stdout/stderr are also captured for diagnostics.
Displays: `text`, `markdown`, `scalar(value, label)`,
`table(columns, rows)`, `image(binary_string, media_type: 'image/png', alt: '')`,
`plotly(hash)`, `vegalite(hash)` and `module(name, hash)`.

MRI and the Card instance stay in the worker: instance variables survive invocations when
`reuseWorker` is enabled. Optional `on_run_start(inputs, report, run)` runs once per Run. Disable reuse for fresh
state. Report and Run expire when the callback returns and may only be used on its thread; copied input
values may be retained. Join any Ruby threads before returning. Exceptions produce a structured
failure; `raise Tsugumi::Failure.new('worker_failed', 'reason')` supplies a diagnostic. `run.cancelled?` and
`run.remaining_ms` support cooperative work; the worker is killed at cancellation / deadline, so
ensure blocks are not guaranteed then. Failed workers are discarded before later work.

On request, create a Gemfile in the Card's directory, then use the project
shell to run `bundle lock --add-platform ruby` from that directory. Reload the
project to read the resulting Gemfile.lock. Name it explicitly in `rubyCard`:
`lock = ./Gemfile.lock; hash = pkgs.lib.fakeHash;`. Run once and replace the
placeholder with the `got: sha256-…` hash of the downloaded `.gem` files from
the Card's build log. Keep both files with the Card and repeat the hash step
when either changes. The locked gems are installed offline, including native
extensions that use libraries supplied through `buildInputs`.
