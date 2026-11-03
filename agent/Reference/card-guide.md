# Writing Tsugumi Cards (for the in-app Agent)

Read with `reference_read`. Every example named here is a working file under
`demos/`; copy from those rather than guessing.

## A Card's folder

`definitions/<id>/` belongs to one Card (its id is its name):

| File | What it is |
| --- | --- |
| `_card.json` | All settings: `language` or `template`, `inputs`, `params`, run settings |
| `_meta.json` | The intent: `description`, `content`, port descriptions (kept when a Draft is implemented) |
| `_outputs.fbs` | One table per output port, root table `Outputs` |
| `default.nix` | Code Cards only: a function `{ pkgs, inputs, params }:` returning `{ run = ...; }` |
| `src/` | Code Cards only: the sources |

Never write `_inputs.fbs`: input types come from the connected outputs.

## Built-in Cards (`"template"` in `_card.json`, no code)

| Template | Ports | Use |
| --- | --- | --- |
| `trigger` | out `out` | Where a whole run starts |
| `input` | in `start` (optional), outs from `_outputs.fbs` | Hands on values or a project file (below) |
| `fork` | in `in`, outs `out1`, `out2` (param `branches`: string[] for other names) | Sends one value to several Cards |
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

Params: `{"name": "low", "type": "double", "data": 600}`; types `bool`, `int64`, `uint64`, `double`,
`string` and their arrays (`double[]`, ...). Python reads them with `ctx.params().Low()`.

### default.nix

- Python: `demos/report/definitions/generate/default.nix`. Packages from nixpkgs:
  `inputs.tsugumi.pythonCard { inherit sdk; src = ./src; packages = ps: [ ps.numpy ps.astropy ps.pillow ]; }`.
- C++: `demos/collatz/externals/collatz-cpp/definitions/halve/default.nix`. Libraries:
  `inputs.tsugumi.cppCard { inherit sdk; src = ./src; buildInputs = [ pkgs.cfitsio ]; }`.
- bash: `demos/report/externals/archive/definitions/copy/default.nix` (`run` is the script text).

### File and Directory outputs

`demos/report/definitions/generate` (File) and `demos/report/definitions/render` (Directory):
`attribute "tsugumi";`, the port marked `(required, tsugumi: "file")` or `(required, tsugumi:
"directory")`, a table with `value:string`, `"filesystem": true`, and the files written under
`ctx.output_directory` (Python) / `$TSUGUMI_OUTPUT_DIR` (bash).

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
