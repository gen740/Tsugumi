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
  Prefer `packages`. A package or version nixpkgs lacks comes from PyPI only through an explicit lock
  (see "Python packages from PyPI" below); never write `pip install` or fetch packages when a Card runs.
- C++: `demos/collatz/externals/collatz-cpp/definitions/halve/default.nix`. Libraries:
  `inputs.tsugumi.cppCard { inherit sdk; src = ./src; buildInputs = [ pkgs.cfitsio ]; }`.
- bash: `demos/report/externals/archive/definitions/copy/default.nix` (`run` is the script text).

### Python packages from PyPI (uv lock)

Nothing is resolved when a Card is built. A Card gets PyPI packages only when its `default.nix` names
its own lock and the hash of the wheels: `pythonCard { inherit sdk; src = ./src; lock = ./uv.lock;
hash = "sha256-..."; }`. The lock belongs to the Card: `pyproject.toml` and `uv.lock` sit beside
`default.nix`. One without the other, a lock that no longer matches `pyproject.toml` and a wrong hash
fail with `nix_failed` and a message. When the user asks for such a package, make the lock yourself:

1. Save `definitions/<card>/pyproject.toml`: `[project]`, `name = "card"`, `version = "0"`,
   `requires-python = ">=3.11"`, `dependencies = ["tabulate==0.9.0"]` (pin exact versions).
2. Make the lock with the project shell (needs Shell access; ask the user to grant it, or to run the
   command in the project's terminal, if you do not have it):
   `cd definitions/<card> && nix shell nixpkgs#uv nixpkgs#python3 --command env UV_PYTHON_DOWNLOADS=never uv lock`.
   The first call downloads uv and may reach the time limit: run it again. Then reload the project
   (the file was written by a command) and read `uv.lock` to check that it names the packages.
3. Save `default.nix` with `lock = ./uv.lock; hash = pkgs.lib.fakeHash;` and run the Card. The Card's build log contains `hash mismatch` and `got: sha256-...`. Save `default.nix` again with
   `hash = "<that value>";` and run. Never invent a hash and never copy one from another Card.
4. To change a dependency, edit `pyproject.toml` and repeat 2 and 3; the old hash is refused.

Only wheels are installed (no wheel for the Card's Python and system: use `packages` from nixpkgs
instead, and say so). Compiled wheels have one hash per system: `hash = { aarch64-linux = "sha256-...";
x86_64-linux = "sha256-..."; };` (a missing system fails and names itself; run on it to get its hash).
A compiled wheel needing another shared library: `libraries = [ pkgs.libGL ];`. Tell the user which
packages and versions the lock pins.

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

### Visualization scaffolds

The Mac/Web creation menu has one Python Card template each for Matplotlib,
Plotly and Altair. The `kind` parameter chooses line, scatter or histogram.
The runnable contract is `demos/en/visualization/definitions/line`. Each
template writes column selection, filtering, sampling and plotting into
`src/main.py`. The SDK's `input_rows` reads the connected `data` port and
`emit_figure` saves plotted values and emits the `figure` Directory.

The Directory includes `plotted.csv` and run, Card and invocation metadata.
Matplotlib saves PNG/SVG/PDF; Plotly and Altair save JSON and offline HTML.
`max_points` fails on over-limit input. Setting `sample_every` above 1
explicitly samples in input order, with the sample size shown in the figure
and metadata. See `docs/Visualization.md` for parameters and boundaries.

### marimo notebooks

The Mac/Web `marimo notebook` starting point is an ordinary Python Card. Its
complete runnable contract is `demos/en/marimo/definitions/notebook`; shared
source files are in `ui/templates/marimo.json`. `src/notebook.py` defines a normal
`marimo.App`. The adapter uses `tsugumi.notebook.bindings(ctx, mapping)` for
`input.<port>.<field>` / `param.<field>` variable bindings and
`run(ctx, source_directory, notebook, definitions, selected)` to execute it once
in a fresh child process. It returns selected JSON-compatible definitions and a
saved Directory. The ordinary adapter packs those values into its declared
output schema only after success.

The example emits a finite number on `value` and retains the Directory on
`report` (`retainArtifact: true`, `artifactOutput: "report"`). `reuseWorker: false`
keeps process lifetime per invocation. Nix dependencies include `ps.marimo` and
`ps.matplotlib`; add other imports there. The static snapshot keeps Markdown/HTML,
text, figures, source and run metadata. Frontend-only widgets have a static note.
There is no interactive server or reactive downstream emission. `docs/Marimo.md`
describes configuration-cell override rules, errors and the future editor-session
boundary. The upstream Card scales 7 by a factor of 2. The notebook visualizes both values and emits 14.

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

## Go Cards

Use `"language": "go"` and start from the whole
`demos/go-pipeline/definitions/scale` folder. The **Go step** menu creates the
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

## R Markdown and Quarto reports

`inputs.tsugumi.reportCard { src = ./src; entry = "report.Rmd"; packages = ps: [ ]; }` runs an R Markdown report; use `report.qmd` for Quarto/knitr. Both produce HTML only. Declare `params: { tsugumi: null }` in the document YAML. R chunks read `params$tsugumi$inputs$data`, `params$tsugumi$params` and File/Directory paths in `params$tsugumi$paths$data`. The adapter returns `report` (Directory) and `html` (File); define both ports, enable `filesystem`, and retain `report` as an artifact. The Mac/Web Add Card menus provide matching scaffolds, and `demos/en/r-reports` / `demos/ja/r-reports` show an aggregation feeding both formats. HTML embeds assets and the saved preview is static; PDF requires a separate pinned toolchain and is not supported. See docs/Reports.md.
