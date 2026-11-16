# Writing Cards

Read with demo_read reference/cards.md. Paths below are repository files; demo_read takes them without demos/
(en/report/definitions/generate/default.nix).

## Implementing a Draft

One project_edit can replace a Draft's definition, add its sources and rebind its wire ends atomically. Existing
_meta.json description, content and context stay as written. The host requires a nonempty description, and context
when the original Draft has a description or content; an empty field can be filled with a guarded metadata edit.
Context records what the person asked and what the Draft says. fill_context is a project_save convenience;
project_edit writes the context field itself. Boundary endpoints in tsugumi.json are editable in the same way.

For example, plan starts with _card.json {"template":"draft"} and _meta.json
{"description":"Make a greeting","content":"Return hello."}, with no context or source files. Its incoming wire is
start:out -> plan:"", and its outgoing wire is plan:"" -> next:"" (next remains a Draft). An end on a Card that stays a Draft keeps port "": only the implemented Card's own ends get real ports. These project_edit
arguments implement plan and retain its intent text and every unrelated project field:

```json
{
  "edits": [
    {
      "file_path": "definitions/plan/_card.json",
      "selector": [],
      "old_value": {
        "present": true,
        "value": {
          "template": "draft"
        }
      },
      "new_value": {
        "present": true,
        "value": {
          "language": "bash",
          "inputs": [
            {
              "name": "start"
            }
          ]
        }
      }
    },
    {
      "file_path": "definitions/plan/_meta.json",
      "selector": [
        {
          "field": "context"
        }
      ],
      "old_value": {
        "present": false
      },
      "new_value": {
        "present": true,
        "value": "The Draft asks for a greeting."
      }
    },
    {
      "file_path": "definitions/plan/_outputs.fbs",
      "put": "namespace greeting;\ntable Greeting { value:string; }\ntable Outputs { result:Greeting (required); }\nroot_type Outputs;\n",
      "expect_missing": true
    },
    {
      "file_path": "definitions/plan/default.nix",
      "put": "{ pkgs, inputs, params }: { run = builtins.readFile ./src/main.sh; }\n",
      "expect_missing": true
    },
    {
      "file_path": "definitions/plan/src/main.sh",
      "put": "printf '{\"result\":{\"value\":\"hello\"}}' > \"$TSUGUMI_OUTPUT\"\n",
      "expect_missing": true
    },
    {
      "file_path": "tsugumi.json",
      "selector": [
        {
          "field": "canvas"
        },
        {
          "field": "connections"
        },
        {
          "match": {
            "from": {
              "card": "start",
              "port": "out"
            },
            "to": {
              "card": "plan",
              "port": ""
            }
          }
        },
        {
          "field": "to"
        },
        {
          "field": "port"
        }
      ],
      "old_value": {
        "present": true,
        "value": ""
      },
      "new_value": {
        "present": true,
        "value": "start"
      }
    },
    {
      "file_path": "tsugumi.json",
      "selector": [
        {
          "field": "canvas"
        },
        {
          "field": "connections"
        },
        {
          "match": {
            "from": {
              "card": "plan",
              "port": ""
            },
            "to": {
              "card": "next",
              "port": ""
            }
          }
        },
        {
          "field": "from"
        },
        {
          "field": "port"
        }
      ],
      "old_value": {
        "present": true,
        "value": ""
      },
      "new_value": {
        "present": true,
        "value": "result"
      }
    }
  ]
}
```

The returned diagnostics and project_validate check the saved graph; run_record checks what a run produced.

## Code Cards

A code Card's _card.json, e.g. {"language": "python", "inputs": [{"name": "config"}], "params": [{"name": "rMin",
"type": "double", "data": 2.5}]}: "language" is python, cpp, go, rust, csharp, bash, r, julia or ruby; "inputs"
names each input port (the "to" port of each incoming wire); a param type is bool, int64, uint64, double or string,
or one of them with [] for a list; code reads a param by its name (inputs.params.rMin), so lowerCamelCase names
read as written. A code Card also has _outputs.fbs (one table per output port; the root table Outputs), default.nix
and its sources in src/. You can use input_types to read the resolved input definitions; pass the intended language for a Draft.

* Data that is already in the project (a file, fixed values) is handed on by a built-in input Card, not code:
  _card.json {"template": "input", "params": [{"name": "value", "type": "string", "data": "<path of the file,
  relative to this Card's definition folder, e.g. data/x.fits>"}]} (its optional "start" input stays wired to the
  trigger), and _outputs.fbs with a File port (one table whose only field is value:string):
      attribute "tsugumi";
      namespace data;
      table EventsFile { value:string; }
      table Outputs { events:EventsFile (required, tsugumi: "file"); }
      root_type Outputs;
  Use project:<path> for a file relative to the Card's own project root (also inside an external project).
  When it runs, the file is copied into the run and the next Card receives its path. Fixed values are params of
  the same names as the fields of its output table (demos/en/report/definitions/settings). Other built-ins: fork,
  branch, merge.
* Otherwise a code Card: _card.json without "template", e.g. {"language": "python", "inputs": [{"name": "config"}],
  "params": [{"name": "rMin", "type": "double", "data": 2.5}]}: "language" is "python", "cpp", "go", "rust",
  "csharp", "bash", "r", "julia" or "ruby"; "inputs" names each input port (the "to" port of each incoming wire; a
  save is refused without it); a param type is bool, int64, uint64, double or string, or one of them with [] for a
  list; name params in lowerCamelCase (code reads them by that name, e.g. inputs.params.rMin). Write
  _outputs.fbs (one table per output port; the root table Outputs), default.nix and the sources in src/.
  default.nix is a function of { pkgs, inputs, params }. Python (demos/en/report/definitions/generate/default.nix):
    { pkgs, inputs, params }:
    let
      sdk = inputs.tsugumi.sdk { languages = [ "python" ]; };
    in
    {
      run = inputs.tsugumi.pythonCard { inherit sdk; src = ./src; };
    }
  Python packages from nixpkgs go in the same call: inputs.tsugumi.pythonCard { inherit sdk; src = ./src;
  packages = ps: [ ps.numpy ps.astropy ps.pillow ]; }. C++ (demos/en/collatz/externals/collatz-cpp/definitions/
  halve/default.nix; libraries: cppCard { ...; buildInputs = [ pkgs.cfitsio ]; }):
    { pkgs, inputs, params }:
    let
      sdk = inputs.tsugumi.sdk { languages = [ "cpp" ]; };
    in
    {
      run = inputs.tsugumi.cppCard { inherit sdk; src = ./src; };
    }
  bash (demos/en/report/externals/archive/definitions/copy/default.nix): run is the script text; inputs are
  $JSON_<PORT> files (jq is available) and $FILE_<PORT> / $DIR_<PORT> paths; outputs are {"port": {...}} JSON
  written to $TSUGUMI_OUTPUT; files go to $TSUGUMI_OUTPUT_DIR:
    { pkgs, inputs, params }:
    {
      # A bash Card: $DIR_REPORT is the input directory and the output port
      # values are written as JSON to $TSUGUMI_OUTPUT.
      run = ''
        dest="$TSUGUMI_OUTPUT_DIR/archive"
        mkdir -p "$dest"
        cp -R "$DIR_REPORT/." "$dest/"
        date -u +%Y-%m-%dT%H:%M:%SZ > "$dest/ARCHIVED_AT"
        echo "archived $(ls "$dest" | wc -l) files"
        printf '{"archive": {"value": "%s"}}' "$dest" > "$TSUGUMI_OUTPUT"
      '';
    }
* Large public data (an observation, a genome) is fixed like a package: fetched once into the Nix store by its hash,
  never during a run. A param with "nix": true is readable in default.nix as params.<name> (changing it rebuilds the
  Card). A bash Card hands the store path on as a plain string (a table { path:string; }, not a File port, whose files
  must be in the output folder), and every Card can read /nix/store:
    { pkgs, inputs, params }:
    let
      raw = pkgs.fetchurl {
        url = "ftp://host/path/${params.file}";  # explicit FTPS: ftp:// with --ssl-reqd (often faster than https)
        curlOptsList = [ "--ssl-reqd" ];
        hash = params.hash;                      # "sha256-..."
      };
      data = pkgs.runCommand "data.fits" { } "gunzip -c ${raw} > $out";
    in
    {
      run = ''
        printf '{"events": {"path": "%s"}}' "${data}" > "$TSUGUMI_OUTPUT"
      '';
    }
* File and folder outputs: declare attribute "tsugumi"; in _outputs.fbs and mark the port (required, tsugumi:
  "file") or (required, tsugumi: "directory"); its table has one field value:string holding the path. Write
  outputs under run.output_directory. demos/en/report/definitions/generate:
    attribute "tsugumi";
    namespace report;
    table CsvFile { value:string; }
    table Outputs { csv:CsvFile (required, tsugumi: "file"); }
    root_type Outputs;
* Python: src/main.py defines main(inputs), main(inputs, report), or main(inputs, report, run). inputs.<port> is a
  read-only object of the connected output's table and inputs.params holds the params; their fields are attributes
  in lowerCamelCase (a field noise_sd is .noiseSd), strings are bytes and numeric vectors numpy arrays. Return each
  output as a dict shaped like its table in _outputs.fbs:
      def main(inputs, report, run):
          path = inputs.events.value.decode()        # a File input is a path
          low = inputs.params.low                    # param "low"
          report.text(f"Input: {path}; lower bound: {low}")
          return {"images": {"value": run.output_directory}}
  The whole SDK (Inputs, Report and Run; generated from their source):
    inputs.<port>, inputs.params.<field> (read-only generated objects)
    return {port: generated_object_or_bytes}; yield mappings for streaming
    inputs.params
    inputs.has_input(port: str)
    inputs.input_bytes(port: str)
    inputs.input(port: str, cls: Any)
    report.text(text: str)
    report.markdown(text: str)
    report.scalar(value: float, label: str)
    report.line_chart(series: Iterable[dict])
    report.table(columns: list, rows: Iterable[Iterable])
    report.image(data: bytes, media_type: str='image/png', alt: str='')
    report.points(x, y, z, colors=None)
    report.figure(*figures, wait: Optional[float]=None)
    report.module(module: str, config, blobs=None)
    report.log(message: str)
    run.run_id
    run.card
    run.invocation
    run.working_directory
    run.temporary_directory
    run.output_directory
    run.cancelled()
    run.remaining_ms()
    tsugumi.builder(size=1024)
    tsugumi.message(value, root=None)
  raise tsugumi.Failure("output_invalid", "why") to fail with a reason; report.log writes a log line.
  Return output mappings; use a generator yielding mappings for incremental batches of distinct ports; each port may be returned once per invocation.
  Use report.figure(fig) for Matplotlib, Plotly and Altair. report.figure() renders open Matplotlib figures
  and drives animations; report.figure(wait=seconds) publishes live frames. Drawing alone never publishes.
  Plotly/Altair interactive plots carry at most 1 MiB of JSON, data inline.
  HOME, XDG_CONFIG_HOME and XDG_CACHE_HOME are writable folders of the worker (libraries such as astropy and
  matplotlib need nothing more).
* marimo: use the shared marimo notebook starting point (ui/templates/marimo.py; demo_read
  en/marimo/definitions/notebook/src/main.py and src/notebook.py). It is an ordinary Python Card with
  ps.marimo in pythonCard packages and reuseWorker: false. Keep the notebook in src/notebook.py;
  the bindings JSON maps notebook variables to inputs.<port>.<field> or param.<field>, e.g.
  {"source":"inputs.data.source","factor":"param.factor"}. Supply every variable of an overridden cell.
  The adapter calls tsugumi.notebook.bindings and run once, then returns
  {"value":{"value":values[selected]},"report":{"value":str(folder)}} against _outputs.fbs
  (numeric value and report Directory, retainArtifact enabled). Notebook cells do not emit ports.
  Interactive sessions are separate (#267).
* C++: sources are `#include <card.hh>` and `auto run(const Card::Inputs& inputs, tsugumi::Report& report) -> Card::Output` with one member per
  port (inputs.<port>, Output's ports are std::optional); visualization is
  report.text/markdown/scalar/line_chart/table/image/points/module.
* Each invocation starts a fresh worker by default. Set `reuseWorker: true` in _card.json to keep this Card's
  state across invocations and runs. To reset that state at the start of each run, use `auto onRunStart(const Card::Inputs& inputs) -> void`
  (Python: on_run_start(inputs, report, run)). Input types come from the connected outputs (there is no _inputs.fbs).
  Optional preRun / postRun are bash without the Card inputs.

## Additional tools

`tools {"family":"files"}` returns file utilities and their argument schemas;
`tools {"family":"sdk"}` returns SDK preparation and flake resolution schemas.
Supply an action and its arguments to call it, for example:

```json
{"family":"runs","action":"status","arguments":{"run_id":"1","wait_seconds":300}}
```

The files `template` action returns a new Card's `definition_json` and sources.
Save them under `definitions/<id>/` together with its canvas instance in one
`project_edit`. Templates use their catalog ids (`python`, `cpp`, `marimo`, etc.).

The files `shell` action runs in the project's environment after a separate Shell
grant. Card paths are project-relative. Attached folders are read-only at
`attached/<first 16 hex digits of SHA-256 of the folder path>/`; `scratch/` persists
for the conversation. Successful source edits save one validated revision. Replies
include bounded stdout/stderr and paths to the full output.

`project_copy` copies attached, Card or conversation scratch files into Card source
paths and returns their sizes and hashes. Data can also stay attached: a Card's
`hostFolders` permission makes it available at `/host/<name>` during execution.

The SDK `resolve_source` action resolves a flake for `nixSources`. To customize its
packages, `nixSources[].overrides` accepts JSON maps such as
`{"packageName":{"override":{"argument":"value"},"overrideAttrs":{"pname":"custom"}}}`.
A value `{"fetchurl":{"url":"https://...","sha256":"..."}}` fetches pinned source
or patches. Cards consume `inputs.sources.<name>.packages.<packageName>` with the
system already selected. Saving broader permissions or changed overrides requests
the person's approval.
