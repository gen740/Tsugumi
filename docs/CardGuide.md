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

R has no FlatBuffers code generator, so an R Card runs in a separate process and exchanges port values as JSON (the worker converts them by the port's type).

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

A Julia Card also runs in a separate process and exchanges values as JSON.

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

It runs with the standard library alone. Packages from the General registry are pinned at build time with `packages = [ "JuMP" ];`. For a figure, pass a Vega-Lite spec as a Dict to `display_vegalite(ctx, spec)` and it is shown as an interactive chart.

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

The default scaffold uses only the standard library and FlatBuffers' Go
runtime from the same pinned nixpkgs source as `flatc`. Its empty `go.sum` is
intentional. For external modules, commit go.mod/go.sum and supply the fixed
module-cache `vendorHash` to `goCard`. Use a fake hash once to obtain Nix's
reported actual hash, then commit that exact hash. Builds use `-mod=readonly`
and a local immutable module cache; Cards never fetch dependencies at runtime.
Native libraries use `buildInputs`, and build tools use `nativeBuildInputs`.
To update dependencies with Go on PATH, run
`python3 <sdk>/go/update-lock.py <card>/src`; it resolves in a temporary copy
and writes only go.mod/go.sum, leaving no generated SDK in the source. The
helper accepts `--offline` for a populated module cache.
