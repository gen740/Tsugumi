# Writing Tsugumi Cards (for the in-app Agent)

Read with `reference_read`. Every example named here is a working file under
`demos/`; copy from those rather than guessing.

## A Card's folder

`definitions/<id>/` belongs to one Card (its id is its name):

| File | What it is |
| --- | --- |
| `_card.json` | All settings: `language` or `template`, `inputs`, `params`, run settings |
| `_meta.json` | The intent, for every kind of Card: `description` (its summary, shown on the Canvas and in Card settings; `_card.json` has none), `content`, `context`, port descriptions. The save implementing a Draft keeps what it has and fills an empty `description` and `context` (only from what the user and the Draft say); `"keep_meta": true` only when nothing is empty |
| `_outputs.fbs` | One table per output port, root table `Outputs` |
| `default.nix` | Code Cards only: a function `{ pkgs, inputs, params }:` returning `{ run = ...; }` |
| `src/` | Code Cards only: the sources |

Never write `_inputs.fbs`: input types come from the connected outputs.

## Built-in Cards (`"template"` in `_card.json`, no code)

| Template | Ports | Use |
| --- | --- | --- |
| `trigger` | out `out` | Where a whole run starts |
| `input` | in `start` (optional), outs from `_outputs.fbs` | Hands on values or a project file (below) |
| `fork` | in `in`, outs `out1`, `out2` (param `branches`: string[] for other names) | Sends one value to several Cards (an output has one wire: this is how a value reaches several inputs) |
| `branch` | ins `value`, `condition` (a table with `value:bool`), outs `a` (true), `b` (false) | Sends to one side |
| `merge` | ins as declared in `inputs`, out `out` (a union of them) | Joins paths of a loop |
| `logger` | in `in` | Shows what it receives (`"markdown": true` shows a text as Markdown) |

### An input Card with fixed values

`demos/report/definitions/settings`: params named like the fields of its output table.

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

The next Card reads it as a path: `ctx.input("events", EventsFile).Value().decode()` (Python),
`$FILE_EVENTS` (bash).

## Code Cards

`_card.json`: `{"language": "python", "inputs": [{"name": "events"}], "params": [...]}`; add
`"filesystem": true` when the Card writes files (File or Directory outputs).

A Card has no internet unless its `_card.json` has `"network": true` (Experimental); then it can
download, for example public data by an id given as a param. TLS uses the environment's CA bundle
(`SSL_CERT_FILE`), so Python's `urllib.request` works as is. Show where the data came from (URL,
size, SHA-256) in the display or the log.

Params: `{"name": "low", "type": "double", "data": 600}`; types `bool`, `int64`, `uint64`, `double`,
`string` and their arrays (`double[]`, ...). Python reads them with `ctx.params().Low()`.

A Card keeps no state: every invocation runs in a new worker (a new process and interpreter), so
globals, statics, loaded modules, `options()` and the random number generator's position never reach the
next invocation or run. Write Cards that way, and pass what the next step needs in a message. Add
`"reuseWorker": true` only when the Card must remember across invocations (a running window of a
stream, say) or is called many times in a loop and should not start an interpreter each time; one
worker then serves later invocations and runs, all of the above carries over, and per-run state is reset
in `on_run_start(ctx)` (C++: `onRunStart()`), called before the first invocation of each run. The working
directory and environment variables are set again for every call either way.

### default.nix

- Python: `demos/report/definitions/generate/default.nix`. Packages from nixpkgs:
  `inputs.tsugumi.pythonCard { inherit sdk; src = ./src; packages = ps: [ ps.numpy ps.astropy ps.pillow ]; }`.
- C++: `demos/collatz/externals/collatz-cpp/definitions/halve/default.nix`. Libraries:
  `inputs.tsugumi.cppCard { inherit sdk; src = ./src; buildInputs = [ pkgs.cfitsio ]; }`.
- bash: `demos/report/externals/archive/definitions/copy/default.nix` (`run` is the script text).

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

`demos/report/definitions/generate` (File) and `demos/report/definitions/render` (Directory):
`attribute "tsugumi";`, the port marked `(required, tsugumi: "file")` or `(required, tsugumi:
"directory")`, a table with `value:string`, `"filesystem": true`, and the files written under
`ctx.output_directory` (Python) / `$TSUGUMI_OUTPUT_DIR` (bash).

### Reading a merge output

The input is a table whose `value` is a union with one member per merge input. C++:
`Card::visit(input.arrival.value, [](const auto& v) { return v.value; })` calls the lambda
with whichever member arrived (`input.arrival.value.type` says which port). Python:
`demos/collatz/externals/collatz-py/definitions/check/src/main.py`.

### Python (`src/main.py`)

```python
from inputs.events.data.EventsFile import EventsFile    # inputs.<port>.<namespace>.<Table>
import outputs.counts.bands.Counts as counts_out          # outputs.<port>.<namespace>.<Table>
from tsugumi import Failure

def main(ctx):
    path = ctx.input("events", EventsFile).Value().decode()
    low = ctx.params().Low()
    b = ctx.builder()
    counts_out.Start(b); counts_out.AddRed(b, 703367)
    ctx.output("counts", b, counts_out.End(b))
    ctx.display_table(["band", "photons"], [["red", 703367]])
```

Also `ctx.display_text`, `display_markdown`, `display_image(png_bytes)`, `display_scalar`,
`display_line_chart`; `raise Failure("output_invalid", "why")`; `print()` goes to the Card's log.

matplotlib (`packages = ps: [ ps.matplotlib ];` in `default.nix`) draws into the Card's display
through the SDK's backend (`sdk/python/tsugumi/mpl.py`), so code written for a window works as it is:
`plt.show()` shows the open figures (several are stacked) and closes them; figures left open when
`main` returns are shown too; `plt.pause()` loops and `FuncAnimation` + `plt.show()` update the
display live while the Card runs (an endless animation ends when the run is stopped).
`ctx.display_figure(fig)` shows a figure with any backend. Japanese labels draw as they are (a
Japanese font is in the Card's package when it uses matplotlib; `pythonCard`'s `fonts` changes it). Keep Japanese and `$...$` math on separate lines of a label: mathtext has no fallback font and draws tofu for Japanese. Do not call `matplotlib.use("Agg")` and
`savefig` just to display. Example: `demos/plots` (a static figure, a live animation, Plotly and Altair).

Plotly (`ps.plotly`) and Altair (`ps.altair`) are interactive plots in the display (zoom, hover,
legend in the expanded view): `fig.show()` / `chart.show()` as written anywhere, or
`ctx.display_figure(fig_or_chart)`. The figure's JSON (Altair: its data inline) is at most 1 MiB;
beyond that, plot fewer points or use matplotlib (an image). Charts cannot load data from URLs.
The whole SDK: `sdk/python/tsugumi/__init__.py`. More: `demos/report/definitions/aggregate`
(reads a File input, makes a table), `demos/collatz/definitions/compare`.

`src/main.py` is imported once when the worker starts, before any `preRun`, and
`preRun` runs before every call. So `preRun`'s exports and `cd` reach `main` (in `os.environ` too)
but not the module's top. Native libraries often read their settings as they load: limit threads
at the top of `main.py`, before the imports, and it lasts for the worker:

```python
import os
os.environ["OMP_NUM_THREADS"] = "1"  # OpenMP and OpenBLAS

import numpy as np
```

What `main` or `on_run_start` sets in `os.environ` lasts for that call only.

### bash

Inputs are `$JSON_<PORT>` (a JSON file of the message), `$FILE_<PORT>` / `$DIR_<PORT>` (paths);
outputs are written as `{"<port>": {...}}` JSON to `$TSUGUMI_OUTPUT`; `jq` is available.

Params are in `$TSUGUMI_PARAMS_JSON` (the JSON text itself). To show something, write one JSON member to
`$TSUGUMI_DISPLAY`: `{"markdown": "..."}`, `{"text": "..."}`, `{"table": {"columns": [...], "rows": [[...]]}}`
(cells are strings), `{"image": {"path": "plot.png", "alt": "..."}}` or `{"module": {"name": "vega-lite", "data":
{...}}}`. It is shown when the script ends successfully.

### R (`src/main.R`)

`_card.json`: `{"language": "r", ...}`; `default.nix`: `run = inputs.tsugumi.rCard { src = ./src; packages = ps: [
ps.survival ps.ggplot2 ]; };` (from nixpkgs' `rPackages`). No `sdk`, no generated classes: messages and params are
lists, converted in memory and checked against the port's FlatBuffers type by the worker.

```r
main <- function(ctx) {
  n <- ctx$input("events")$value          # a message; ctx$path("events") for a File / Directory
  ctx$output("counts", list(red = 703367, bins = I(c(1, 2))))  # I(): a field that is an array, whatever its length
  ctx$plot(plot(density(x)), alt = "...") # base graphics or a ggplot, as the Card's image
  ctx$display_table(df)                   # or display_text, display_markdown, display_image(path), display_module(name, spec)
}
```

`ctx$params$low`, `ctx$output_directory`, `ctx$temporary_directory`. `stop("why")` fails the Card and
goes to its log; `print()` and `message()` go to the log. One display: the last shown. Call `set.seed` yourself: a
new interpreter starts unseeded, and with `reuseWorker: true` a later run continues the earlier run's random sequence. An
integer outside the exact double range (±(2^53 − 1)) is read as a decimal string, including the full signed and unsigned 64-bit ranges.
A `long`/`ulong` field takes an R integer (`27L`) or a string of digits (`sprintf("%.0f", x)`), not a double: `27` is written as
`27.0` and the type rejects it.

### Julia (`src/main.jl`)

`_card.json`: `{"language": "julia", ...}`; `default.nix`: `run = inputs.tsugumi.juliaCard { src = ./src; };`, with
the standard library. The existing `packages = [ "JuMP" ];` path uses nixpkgs import-from-derivation and is
currently rejected by the Card evaluator; do not rely on registry package resolution.

```julia
function main(ctx)
    n = input(ctx, "events")["value"]     # a Dict; path(ctx, "events") for a File / Directory
    output(ctx, "counts", Dict("red" => 703367))
    display_vegalite(ctx, spec)           # a Vega-Lite spec as a Dict, data inline: an interactive chart
    display_markdown(ctx, "**done**")     # or display_text, display_table(ctx, columns, rows), display_image(ctx, path; alt=""), display_module
end
```

`ctx.params["low"]`, `ctx.output_directory`, `ctx.temporary_directory`. A thrown error fails the Card and goes to its
log. No plotting library is included: use Vega-Lite (at most 1 MiB of JSON), or add one by `packages`.

## Checking and running

1. Save with `project_save`; its result lists each Card's diagnostics.
2. When the writing is done, check once with `project_validate` (it resolves the latest revision) and fix what it
   reports.
3. Run from the trigger Card (`run` with the trigger's id); a single Card run only works on inputs
   saved by an earlier whole run.
4. `run_status` until it ends, then `run_record` of each Card; `log_range` (stream 2) for errors.

## C# / .NET

Set `language: "csharp"`. The Card body is a public `Card : Tsugumi.ICard` in `src/Card.cs`; implement `ValueTask RunAsync(Context context)`. Select the generated typed bindings and pinned runtime in `default.nix`:

```nix
{ pkgs, inputs, params }:
let sdk = inputs.tsugumi.sdk { languages = [ "csharp" ]; };
in { run = inputs.tsugumi.csharpCard { inherit sdk; src = ./src; }; }
```

`context.Inputs.<Port>` unpacks the generated object API (`inputs.<port>.<schema namespace>.<Table>T`); optional ports are nullable. `context.Parameters` is `params.ParamsT`; scalar fields are nullable, so use `??` with a default or throw `new Failure("validation_failed", "missing parameter")`. Emit each port with `context.Outputs.<Port>(new outputs.<port>.<namespace>.<Table>T { ... })`. Port names capitalize their first character. Read the exact schema instead of guessing namespaces.

`DisplayText`, `DisplayMarkdown`, `DisplayScalar`, `DisplayTable`, `DisplayImage`, `DisplayLineChart`, `DisplayPoints`, `DisplayModule` and `Log` write into the current invocation. File/Directory messages contain `Value` paths; write under `OutputDirectory`, then emit the generated message. Text must be valid UTF-8 without embedded NUL. Input/param bytes are managed copies; native callbacks on a retained Context reject use after its invocation.

Await all work belonging to `RunAsync`; pass `context.CancellationToken` to cancellable Tasks. Fire-and-forget Tasks are not joined. `OnRunStartAsync(Context)` resets per-run state; the Card instance and CLR persist across invocations only with `reuseWorker: true`. Exceptions produce structured failures and do not cross the native ABI.

NuGet packages need both `lock = ./packages.lock.json;` (NuGet's own lock file, in the Card's folder) and `hash = "sha256-...";` in `csharpCard`. The lock is the only package list (its `Direct` entries become the references); `hash` is the NAR hash of the `.nupkg` files the lock names, fetched as `<name>-nuget-lock-<lock id>`. The build restores offline in locked mode; nothing is fetched while a Card runs. Never invent a lock, a `contentHash` or a `hash`, and do not add packages on your own: the person chooses them. The bundled `demos/csharp-calibration/definitions/calibrate` is a complete typed, parameterized example with a locked dependency, between C++ and Python Cards.

When asked to make or update a lock:

1. In the project shell, create a temporary .NET project using the Card's
   framework (net10.0 by default). Put the requested packages in
   `PackageReference` items and set `RestorePackagesWithLockFile=true`. For
   example, `<PackageReference Include="Newtonsoft.Json" Version="13.0.3" />`.
2. From that temporary directory, run
   `nix shell nixpkgs#dotnet-sdk_10 -c env DOTNET_GENERATE_ASPNET_CERTIFICATE=false dotnet restore`.
   Copy the resulting `packages.lock.json` beside the Card's `default.nix`,
   reload the project and confirm the resolved versions.
3. Give `csharpCard` the arguments `lock = ./packages.lock.json; hash = pkgs.lib.fakeHash;`
   and run the Card. Copy the `got: sha256-…` value from that Card's build log
   (`log_range`, stream 4) into `hash`. Run again to check the offline restore.
   Keep the lock and hash together when updating dependencies.
