# Writing Tsugumi Cards (for the in-app Agent)

Read with `reference_read`. Every example named here is a working file under
`demos/`; copy from those rather than guessing.

## A Card's folder

`definitions/<id>/` belongs to one Card (its id is its name):

| File | What it is |
| --- | --- |
| `_card.json` | All settings: `language` or `template`, `inputs`, `params`, run settings |
| `_meta.json` | The intent: `description`, `content`, port descriptions (kept when a Draft is implemented: `PutDefinition` with `"keep_meta": true`) |
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

`_card.json` (the path is relative to the project):

```json
{"template": "input",
 "params": [{"name": "value", "type": "string", "data": "definitions/load/data/events.fits"}]}
```

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
Japanese font is in the Card's package when it uses matplotlib; `pythonCard`'s `fonts` changes it). Do not call `matplotlib.use("Agg")` and
`savefig` just to display. Example: `demos/plots` (a static figure, a live animation, Plotly and Altair).

Plotly (`ps.plotly`) and Altair (`ps.altair`) are interactive plots in the display (zoom, hover,
legend in the expanded view): `fig.show()` / `chart.show()` as written anywhere, or
`ctx.display_figure(fig_or_chart)`. The figure's JSON (Altair: its data inline) is at most 1 MiB;
beyond that, plot fewer points or use matplotlib (an image). Charts cannot load data from URLs.
The whole SDK: `sdk/python/tsugumi/__init__.py`. More: `demos/report/definitions/aggregate`
(reads a File input, makes a table), `demos/collatz/definitions/compare`.

### bash

Inputs are `$JSON_<PORT>` (a JSON file of the message), `$FILE_<PORT>` / `$DIR_<PORT>` (paths);
outputs are written as `{"<port>": {...}}` JSON to `$TSUGUMI_OUTPUT`; `jq` is available.

## Checking and running

1. Check planned changes with `project_validate {"changes": [...]}` (nothing is saved).
2. Save with `project_save`.
3. Run from the trigger Card (`run` with the trigger's id); a single Card run only works on inputs
   saved by an earlier whole run.
4. `run_status` until it ends, then `run_record` of each Card; `log_range` (stream 2) for errors.
