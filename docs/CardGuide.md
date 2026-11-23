# Your first Card

Make a C++ Card that converts a temperature from Celsius to Fahrenheit. Then rerun it with a different parameter and inspect the saved result. The [temperature-conversion template](https://github.com/gen740/tsugumi-kit/tree/main/templates/temperature-conversion) has the same wiring.

## What you will make

```text
start ──out→ settings ──temperature→ convert
```

The Trigger starts the run. The Input Card `settings` sends an editable temperature. The Code Card `convert` calculates a Fahrenheit value, sends it to its live view and returns a typed output.

<div data-player="card-guide"></div>

## Making it on screen

1. Create a project from the temperature-conversion template or start with an empty project.
2. For an empty project, place a Trigger, an Input Value and a Draft on the Canvas. Name them `start`, `settings` and `convert`. Add the files below and connect the ports. In the Draft's settings, choose Code as the implementation kind.
3. If you are declaring the sources yourself, add the two names under **Project settings → Name and Environment → Nix search path**. Choose **Update all** to write `tsugumi.lock`.

## The files

`tsugumi.json`

```json
{
  "name": "Temperature conversion",
  "nixPath": [
    {"name": "nixpkgs", "url": "github:NixOS/nixpkgs/nixos-unstable"},
    {"name": "tsugumi-kit", "url": "github:gen740/tsugumi-kit/main"}
  ],
  "canvas": {
    "cards": [{"id": "start"}, {"id": "settings"}, {"id": "convert"}],
    "connections": [
      {"from": {"card": "start", "port": "out"}, "to": {"card": "settings", "port": "start"}},
      {"from": {"card": "settings", "port": "temperature"}, "to": {"card": "convert", "port": "celsius"}}
    ]
  }
}
```

The lock below is the template's checked-in example. Use an explicit update to resolve new pins for your own declarations. [Project Nix search path](features.md#project-nix-search-path)

`tsugumi.lock`

```json
{
  "nixPath": [
    {
      "name": "nixpkgs",
      "url": "github:NixOS/nixpkgs/nixos-unstable",
      "locked": {
        "type": "github",
        "owner": "NixOS",
        "repo": "nixpkgs",
        "rev": "1c89083e451e891152365b799595cf7790a43325",
        "narHash": "sha256-Y1CiZ4SoBUjLQyQM/pY+rS2dXD3CHLPstF0lmOc73D4=",
        "lastModified": 1793341285
      }
    },
    {
      "name": "tsugumi-kit",
      "url": "github:gen740/tsugumi-kit/main",
      "locked": {
        "type": "github",
        "owner": "gen740",
        "repo": "tsugumi-kit",
        "rev": "54bf325572c0a66e5924b5cd091597975bcafbe3",
        "narHash": "sha256-LLpRNabK7h3i3Ve9hO6WZFIief86ZGPJOqS7YeW8lqg=",
        "lastModified": 1795417034
      }
    }
  ]
}
```

`definitions/start/_card.json`

```json
{"type": "trigger"}
```

### The input value Card

The run parameter has the same name as the field in the `Temperature` message. The `temperature` field of `Outputs` names the output port.

`definitions/settings/_card.json`

```json
{"type": "input"}
```

`definitions/settings/params.nix`

```nix
{
  run = {
    value = 20.0;
  };
}
```

`definitions/settings/_outputs.fbs`

```text
namespace thermometer;
table Temperature { value:double; }
table Outputs { temperature:Temperature (required); }
root_type Outputs;
```

### The conversion Card

`definitions/convert/_card.json`

```json
{"type": "code", "inputs": [{"name": "celsius"}], "params": {"plot": true}}
```

`definitions/convert/_outputs.fbs`

```text
namespace thermometer;
table Temperature { value:double; }
table Outputs { fahrenheit:Temperature (required); }
root_type Outputs;
```

`definitions/convert/params.nix`

```nix
{
  run = {
    scale = 1.8;
    offset = 32.0;
    unit = "°F";
  };
  build = {
    cxxflags = [ "-O2" ];
    plot = false;
  };
}
```

`definitions/convert/default.nix`

```nix
{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/cpp> { inherit pkgs; };
  common = {
    inherit inputs;
    outputs = ./_outputs.fbs;
    params = ./params.nix;
  };
in
{
  run = sdk.cppCard (common // {
    src = ./src;
    cxxflags = params.cxxflags;
    tools = pkgs.lib.optional params.plot pkgs.gnuplot;
  });
  api = sdk.cppApi common;
  views = builtins.filter (view: view.name == "markdown") sdk.views;
}
```

The override for `plot` adds gnuplot to the Card's available tools. `scale`, `offset` and `unit` are delivered on each call. The SDK creates types from the port schema, `_outputs.fbs` and the run declarations in `params.nix`.

`definitions/convert/README.md`

```markdown
---
description: Convert Celsius to Fahrenheit
---
```

`definitions/convert/src/card.cc`

```cpp
#include "card.hh"

auto run(const Card::Inputs& input) -> Card::Output {
  using Fallback = decltype(Card::Output::fahrenheit)::value_type;
  using Celsius = Card::InputType<"celsius", Fallback>;
  const auto celsius = input.get<Celsius>("celsius");
  const double fahrenheit = celsius.value * input.params.scale + input.params.offset;
  card::display::send(card::display::Scalar{fahrenheit, input.params.unit});
  Card::Output output;
  output.fahrenheit.emplace().value = fahrenheit;
  return output;
}
```

`Card::InputType` selects the connected type or a fallback that lets the source compile while unwired. `input.get` reports `input_missing` if the call requires an absent value. In the Card editor's Source view, press **Build SDK** to read the generated names and declarations.

## Running it and checking

1. Press **Run** (⌘R). The first build prepares the Card's environment.
2. Open `convert`'s live view to see `68 °F`. Use **Run History** to inspect the output on `fahrenheit` and the logs for evaluation, build and execution.
3. Change the `offset` to `0.0` and choose **Run Only This Card (Last Inputs)**. This experiment uses the saved 20-degree input and yields 36 without rebuilding the Card. Resetting the parameter removes its override and restores `32.0`.

## Inputs and outputs

A Card is `(input) -> output`. One call receives the values on its input ports and its parameters, and returns the values for its output ports. That is all that crosses between the Card and Tsugumi: there is no run ID, Card ID or invocation number, no hook before or after the call, and no report object. What the Card shows while it works goes to its [views](#views); what it writes to stdout and stderr goes to its log.

| SDK in `default.nix` | Entry point | Guide |
| --- | --- | --- |
| `cppCard` | `auto run(const Card::Inputs& input) -> Card::Output` | [C++](https://github.com/gen740/tsugumi-kit/blob/main/docs/cpp-sdk.md) |
| `pythonCard` | `def run(input: Input) -> Output` | [Python](https://github.com/gen740/tsugumi-kit/blob/main/sdk/python/README.md) |
| `rCard` | `main <- function(input)` returns a named list | [R](https://github.com/gen740/tsugumi-kit/blob/main/sdk/r/README.md) |
| `juliaCard` | `main(input)` returns a NamedTuple or Dict | [Julia](https://github.com/gen740/tsugumi-kit/blob/main/sdk/julia/README.md) |
| `goCard` | `func run(input tsugumi.Inputs) (tsugumi.Output, error)` | [Go](https://github.com/gen740/tsugumi-kit/blob/main/sdk/go/README.md) |
| `rustCard` | `fn run(input: Inputs<'_>) -> Result<Output, Failure>` with `tsugumi::export!(run)` | [Rust](https://github.com/gen740/tsugumi-kit/blob/main/sdk/rust/README.md) |
| `rubyCard` | `def main(input)` returns a Hash | [Ruby](https://github.com/gen740/tsugumi-kit/blob/main/sdk/ruby/README.md) |
| `csharpCard` | `public static Output Run(Input input)` | [C#](https://github.com/gen740/tsugumi-kit/blob/main/sdk/csharp/README.md) |

Any SDK that implements the [Card ABI](CardABI.md) can take the place of these: the core knows no language, only a built `lib/main.so` that exports `tsugumi_main`.

The core supplies connected input types as `inputs.<port>.type`. The SDK combines them with `_outputs.fbs` and `params.nix` to generate the typed interface. **Build SDK** builds `card.api` via the [generic Nix evaluation tool](features.md#evaluating-a-card-expression) and shows the files under `share/tsugumi/api/`.

- An input declaration contains its name. Connect one wire to give it the upstream output’s type. An unconnected input is absent. The Card’s SDK reports `input_missing` with the port name when the Card requires an absent input.
- Run parameters arrive in one `parameters.Params` message, with every field set to its effective value.
- The Card returns one value per output port it produces. A missing required output fails the call with `output_invalid`. The SDK checks the payload against the port's type.
- Numeric messages keep IEEE NaN, positive and negative infinity and signed zero.

### Building outputs

A Path port carries the path directly.

- **A new file or directory.** Create it under the Card's working directory, finish writing and close it **before returning**, then return its path. A relative path such as `filtered.csv` works. Directories may contain regular files and subdirectories. The worker rejects symlinks, paths under `TMPDIR` and granted host paths as new outputs.
- **Forwarding an input**: return the input's path unchanged. Tsugumi records the stored original without copying it.

The worker snapshots the new files after the call returns. Each call has limits on the total bytes and number of entries it outputs, set in the project's settings (64 GiB and 1,000 entries by default; forwarded inputs do not count). Exceeding them fails the call with `output_invalid`. [Run history](RunHistory.md#keep-and-open-artifacts) describes how outputs are kept.

### Failures and cancellation

Raise or return the SDK's failure (Python `tsugumi.Failure("validation_failed", "why")`, C++ `tsugumi::Failure(kind, message)` and so on) to fail the call with a kind, a message and, where the SDK can tell, the source file and line. Any other uncaught error fails with `worker_failed`. A failed call's outputs are discarded and its worker is not reused.

During a call, the Card can ask whether it has been cancelled and how much of its deadline remains (`cancelled()` and `remaining_ms()` in each SDK). A long loop should check between units of work. A stop or deadline also ends code that does not check.

## Views

A Card shows what it is doing in its views. A view is fixed by the Card's definition, before any run: the Card declares its views in `default.nix`, next to `run`, `api` and `exports`, and the Card's view window shows each of them, empty, until data arrives. While the Card runs it sends messages to a view by name; the view draws them.

```nix
{
  run = sdk.pythonCard (common // { src = ./src; });
  api = sdk.pythonApi common;
  views = sdk.views ++ [
    { name = "notes"; type = "html"; blob = ./notes.html; }
  ];
}
```

| Field | Meaning |
| --- | --- |
| `name` | The name the Card sends to |
| `type` | `"html"`: one self-contained HTML file. `"dawn"`: one Wasm binary drawn with WebGPU |
| `blob` | That one file, as a path or a derivation in the Nix store |

The SDK's own views are added explicitly with `sdk.views` (or a part of it, with `builtins.filter`); nothing is added for you. The C++ SDK bundles html views for text, Markdown, tables, images, line charts (`LineChart`, series of x/y) and points (`Points`, x/y/z with optional colours), and the Python SDK the same and Plotly. Each SDK's guide lists the views it bundles and the values it encodes for them. A Card that never sends declares no views.

Where an SDK renders text into images (Matplotlib figures in the Python SDK, for example), it uses IPAexGothic from the kit as the Japanese fallback font, so Japanese labels render offline.

### Sending

The Card ABI gives the Card one effect, `send(view, bytes)`. The bytes are opaque to Tsugumi: their format is a private agreement between the code that sends and the view that reads. Each SDK has a raw form and typed forms that encode a value for one of its bundled views:

| Language | Raw bytes | A typed value |
| --- | --- | --- |
| C++ | `card::display::send("notes", bytes)` | `card::display::send(card::display::Table{columns, rows})` |
| Python | `tsugumi.send("notes", data)` | `tsugumi.send(tsugumi.Markdown("**42**"))`, `tsugumi.send(tsugumi.mpl.figure(fig))` |
| R, Julia | `send("notes", bytes)` | `send(value)` |
| Go | `tsugumi.Send("notes", data)` | `tsugumi.SendValue(value)` |
| Rust | `tsugumi::send("notes", data)` | `tsugumi::send_value(value)` |
| Ruby | `Tsugumi.send("notes", data)` | `Tsugumi.send(value)` |
| C# | `Effects.Send("notes", data)` | `Effects.Send(value)` |

- The core passes the message through without looking at it: it adds the Card and the run that is active when the message arrives and hands it to the UI. It does not check the view name. The UI matches the name against the Card's declared views and shows an error for a name the Card did not declare.
- `send` stays valid for the life of the Card's process. A thread the Card started can keep sending after the call returns, for example to show progress of work that continues between calls.
- When the UI falls behind, it keeps only the latest message for each view. Views also hear when a run that includes the Card starts and ends.
- **Nothing a Card sends is recorded.** A view is for watching the work as it happens; it is not saved with the run, and it cannot be reopened from Run History, compared between runs or read by the Agent afterwards. A value people should check later belongs in an output port, a file or an artifact.

### HTML views

An `html` view is one self-contained HTML file: scripts, styles, fonts, images and any Wasm are inlined. The UI loads it in an isolated frame with no network access (`connect-src 'none'`), so nothing can be fetched at run time.

The page draws its own empty state when it loads. The UI then posts one window message, `{tsugumi: 1, port}`, transferring a `MessagePort`. Everything else arrives on that port:

| Message | Meaning |
| --- | --- |
| `{type: "data", bytes: ArrayBuffer}` | One message the Card sent to this view |
| `{type: "run", state: "started"}` / `{type: "run", state: "ended"}` | A run that includes this Card started or ended |

```html
<!doctype html>
<meta charset="utf-8">
<main>No data yet</main>
<script>
  const main = document.querySelector('main')
  window.addEventListener('message', event => {
    if (event.data?.tsugumi !== 1) return
    event.data.port.onmessage = ({ data }) => {
      if (data.type === 'data') main.textContent = new TextDecoder().decode(data.bytes)
      if (data.type === 'run') main.dataset.run = data.state
    }
  })
</script>
```

The view owns its layout and resizing. To bundle libraries into one file, build the HTML with Nix (the kit's [bundled views](https://github.com/gen740/tsugumi-kit/tree/main/sdk/views) use esbuild in a `runCommand`) and give the derivation as `blob`. [Showing Card views](security.md#showing-card-views) describes their isolation.

### Dawn views

A `dawn` view is a Wasm binary that draws with WebGPU through the pinned `webgpu.h` and nothing else. The UI does not host it yet: a declared `dawn` view shows a placeholder. Its exports are being fixed in the [Card ABI](CardABI.md).

## Worker state

Cards start a fresh worker for each call by default. Set `reuseWorker: true` to keep one worker for the Card: memory, loaded libraries and files in its working directory survive across calls and runs. One worker takes one call at a time. There is no hook at the start of a run, so a Card that keeps state decides from its inputs when to reset it, for example starting over when a loop's initial value arrives. Resident workers have no count, idle-time or memory eviction limit, including workers used for monitoring. They live until the project's environment stops or the Card's definition, SDK, grants or a secret value changes; a call already running when the definition changes finishes on the old version. A failed, cancelled or timed-out call always discards its worker; the next call starts fresh and logs the reason, as it does after a crash. Each invocation records whether its worker already held state (`worker_reused`). Ordinary fresh workers prepared for speed remain limited to 32, ten minutes idle and 2 GiB each.

| Location | Use | Lifetime |
| --- | --- | --- |
| Working directory, also `HOME` | The Card's files and caches. New outputs are created here. | The worker. A reused worker keeps the contents. |
| `TMPDIR` | Scratch. Read the path from the environment. | Cleared for each call. |
| `/tmp` | On Linux workers, the shared VM and remote workers, the same scratch as `TMPDIR`. | On native macOS only `TMPDIR` is scratch. |

The worker sets the working directory, `HOME`, `TMPDIR` and the environment at launch. A file input's path is valid only during its call. A cache in `HOME` should be safe to rebuild when a later call finds it missing. The Card's process gets no shell and no `PATH` of its own: there is no implicit `/bin/sh`. A Card that runs other programs declares them to its SDK's builder, for example `sdk.pythonCard (common // { src = ./src; tools = [ pkgs.coreutils pkgs.ripgrep ]; })`, and the builder puts exactly those on the Card's `PATH`. Packages come from the project's pinned nixpkgs.

## Card definition reference

### A Card's folder

`definitions/<id>/` belongs to one Card. The permanent id is the Card's name and identifies it in canvas keys, connections, saved inputs, run records and evidence links. The core's `card_id` RPC reserves a lowercase slug from the supplied stem, adding `-2`, `-3` and so on as needed. Pass `project_path` to reserve an id inside a Card in Card.

The core checks `tsugumi.json`, `_card.json` and `_comments.json` against the
[project](../protocol/project_document.fbs), [Card](../protocol/card_document.fbs)
and [review comment](../protocol/comments_document.fbs) schemas. Saves use a
two-space indent and schema field order. The UI stores Card placement, flow,
groups and note geometry in [`layout.bin`](../protocol/layout_document.fbs).
Each note's text is Markdown in `notes/<id>.md`.

Definition files:

| File | What it is |
| --- | --- |
| `_card.json` | What the Card is (`type`, required) and its settings, including `inputs`, parameter overrides and run settings |
| `README.md` | Optional one-line `description: …` between `---` lines and a Markdown body. Keep the description and body when implementing a Draft. |
| `_comments.json` | Review comments, keyed by stable comment ids across versions. |
| `_outputs.fbs` | One table per output port, root table `Outputs` |
| `params.nix` | Optional parameter declarations and their defaults |
| `default.nix` | Code Cards only. A function `{ params, inputs }:` returning `run` and optional `views`, `exports` and `api` |
| `src/` | Code Cards only: the sources |

Input types come from the connected outputs. Select the SDK in `default.nix`.

### Card types

Every `_card.json` states what the Card is in `type`; a missing or unknown `type` is a definition error.
`code` runs the Card's own sources (below), `draft` is a step described but not written yet, and `subgraph` is a Card in Card (with `external`).
The other types are built-in Cards with no code:

| Type | Ports | Use |
| --- | --- | --- |
| `trigger` | out `out` | Where a whole run starts |
| `input` | in `start`, outs from `_outputs.fbs` | Passes on values or a project file or folder |
| `branch` | ins `value`, `condition` (a [Condition buffer](CardABI.md#built-in-values)), outs `a` (true), `b` (false) | Sends to one side |
| `fork` | in `value`, outs as declared in `outputs` (default `a`, `b`) | Sends the same value to every output |
| `merge` | ins as declared in `inputs`, out `out` | Emits each arrival unchanged when every connected input has the same type. Mixed FlatBuffer inputs produce one union member per distinct type |
| `collect` | in `in`, out `out` | Sends a batch as one `Collected` buffer of opaque port envelopes |

Use one wire per input port. Route several paths into separate inputs of a Merge Card, then connect its output to the destination. Such a Card can print the value to stdout, which goes to its log, and send it to a `text` view.

### An input Card with fixed values

`demos/en/report/definitions/settings`: params named like the fields of its output table.
To control a Branch from the Canvas, declare a `bool` parameter named `value`
and an output message with a `value:bool` field. Wire that output to the
Branch’s `condition` input. The Canvas renders the parameter as a toggle.

```nix
{ run = { seed = 42; }; }
```

### An input Card that passes on a project file or folder

The editor has a picker for files and folders in the project. At run time the
worker stages a copy and the next Card receives its path.

Set `"type": "input"` in `_card.json`. Declare the path in the Input Card's `params.nix`:

```nix
{ run = { value = "data/events.fits"; }; }
```

The path is relative to the Card's definition folder.

Use `project:definitions/shared/data/events.fits` for a saved source in another
Card of the owning project.
The same rules apply inside external projects: `project:` names that external
project's root. Paths cannot be absolute or contain `..`, and must name a saved
project file or a folder containing saved sources. The base never changes according to which file exists.

`_outputs.fbs` (a Path port):

```
attribute "tsugumi";
table Outputs { events:string (required, tsugumi: "path"); }
root_type Outputs;
```

The next Card reads the path directly, for example `Path(input.events)` in Python.

### Parameters

Declare defaults in the optional `params.nix` in the Card's definition folder. The top-level attrset may contain `run`, `build`, or both. A parameter name may appear in just one section. Names use 1–128 ASCII letters, digits, underscores, hyphens or dots, excluding `.` and `..`. Run names must also be valid FlatBuffers field names, so identifiers such as `sample_rate` work across SDKs.

The default determines the type. A default can be a boolean, signed 64-bit integer, double-precision float, literal string or a nonempty homogeneous list of those scalars. `10` is an integer and `10.0` is a float. Use a decimal point in exponent notation, as in `1.0e3`, and parenthesize a negative list element, as in `[ 0 (-3) ]`. Declarations contain literals and the two section attrsets. Expressions, interpolation, paths, nested parameter objects, nested lists and null are invalid. The core validates the parsed declaration and caches it by file content.

For each name, the effective value is its default unless `_card.json` contains an override, for example `"params": {"offset": 0.0}`. JSON numbers can override a float. An integer override must be integral. Undeclared names and incompatible values are validation errors. Resetting the parameter removes the override. The editor leaves the declarations in `params.nix` intact.

| Section | Where the value arrives |
| --- | --- |
| `run` | `CardInput.params` on each invocation, as `parameters.Params`. The fields have IDs assigned in bytewise name order. A value edit does not change the build inputs. |
| `build` | `params.<name>` in `default.nix`, as a typed Nix value. Use it for settings that affect derivations. |

### Code Card settings

A Code Card has `"type": "code"`. Declare input port names in `inputs`. The connected outputs supply the input types, and `default.nix` selects the SDK.

Network access is off by default where the worker can isolate it. Set `grants.network` to true to allow it. TLS uses the Nix `cacert` bundle through `SSL_CERT_FILE`; report downloaded data's URL, size and SHA-256. For immutable public data, prefer a pinned Nix fetch as described below.

Use `executionService` to select a registered service. The default is `auto`. In `sandbox`, `filesystem`, `pid`, `ipc` and `identity` default to true. Use `sandbox.limits.cpu` for a limit in cores (fractions are allowed) and `sandbox.limits.memoryMiB` for a memory limit in MiB. Omit either value or use `null` for an unlimited resource. A Linux worker with delegated cgroup v2 applies these limits to the Card's process.

Set `grants.network` or `grants.gui` to enable that access on a supported worker. Add named paths with `grants.hostPaths` entries containing `name`, an absolute `path` and an optional `write` flag. The Card reads the worker-visible path from `TSUGUMI_GRANT_<NAME>`, using uppercase letters and underscores in place of other separators. For example, `sample-data` becomes `TSUGUMI_GRANT_SAMPLE_DATA`.

Connection details and credentials belong to the host; see [Remote execution](RemoteExecution.md). Worker state follows the [worker state rules](#worker-state).

### Card secrets

A Card that needs a credential declares only its names: `"secrets": ["database_password"]`. A name is `[a-z][a-z0-9_]{0,63}`. Set the value in the project's **Settings > Card secrets** in the Mac app or the Web UI; this host's hostd keeps it for that project only and never shows it again. At launch the Card's process receives it as an environment variable of the same name, the same way in every language:

```python
import os
password = os.environ["database_password"]
```

The value is not in the project, its parameters, its Nix build or the run history; the history records only that `database_password` was provided. A Card whose secret is not set fails before it runs and names the missing one. When a secret changes, the next run starts a fresh worker. A [remote worker](RemoteExecution.md#card-secrets) receives secrets at launch too.

What the Card does with the value is its author's responsibility: child processes inherit the environment, and a value the Card prints, logs or writes to an output is recorded like any other. Pass it on only where it is needed (to a subprocess, in its environment rather than its command line), and never send it to a view or write it to an output. See [Card secrets](security.md#card-secrets).

### Run lists and connected Nix exports

Use `demos/en/signal-sweep` for a parameter sweep. The `samples` Input Card
sweeps its declared run parameter with `"sweeps": {"value": [8, 12, 16]}` in `_card.json`. It hands
on each signal in its own invocation, labeled `value=8` and so on.

A Code Card compares implementations with a run list: `run` is a list of
`{ label; run; }`, one built Card per entry. The label names the implementation
in the history and when comparing runs. Labels are unique within a list. The
`score` Card receives `samples.out` on its `signal` port and builds one
implementation per threshold from the same source, with the threshold written
beside `main.py` as `threshold.py`:

```nix
{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/python> { inherit pkgs; };
  common = { inherit inputs; outputs = ./_outputs.fbs; };
  thresholds = [
    10
    14
  ];
  compare = threshold: {
    label = "threshold=${toString threshold}";
    run = sdk.pythonCard (common // {
      src = pkgs.runCommand "score-threshold-${toString threshold}" { } ''
        cp -r ${./src} $out
        chmod -R +w $out
        echo "THRESHOLD = ${toString threshold}" > $out/threshold.py
      '';
    });
  };
in
{
  run = map compare thresholds;
  api = sdk.pythonApi common;
  exports.thresholdCount = builtins.length thresholds;
}
```

A list-valued run parameter is one ordinary vector. Under `sweeps`, each entry in a list-typed sweep is one vector. A one-member sweep still forms a batch. Multiple sweeps produce their Cartesian product with the last parameter varying fastest. Code Cards compare implementations through run lists.

The three swept values and two implementations produce six invocations, in input
item order with implementation order varying fastest. Every batch input is an
independent axis. `maxRuns` counts triggers, and a failed member cancels the
rest of the batch. Inspect each member's history for its inputs and result.
An empty list yields an empty batch.

Add a built-in `collect` after `score` to send one ordinary port value with an
`items` vector, which the `summary` Card reads on its `results` port
(`input.results.items`). Each item carries its name and kind, with the original
bytes or path. See the fixed [Collected layout](CardABI.md#built-in-values).

Refer to Nix exports as `inputs.<destination-port>.exports`. Collect, Fork, Branch's
`value` input and Workflow boundaries preserve a unique Code producer. Use a
direct connection to that Code Card if the producer would otherwise be
ambiguous. The `score` Card exports `thresholdCount`, which `summary` reads through
Collect as `inputs.results.exports.thresholdCount` and writes beside its own source the
same way. Each run implementation shares the same exports.

### Data fixed by Nix

Large public data (an observation, a reference genome, a simulation's result) can be fixed like a
package: fetched once into the Nix store by its hash, before a run. Declare the file name and hash as `build` parameters in `params.nix` and read them as `params.<name>` in `default.nix`. The store
path reaches the Card's code through a small generated module beside its source:

```nix
{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/python> { inherit pkgs; };
  common = { inherit inputs; outputs = ./_outputs.fbs; params = ./params.nix; };
  raw = pkgs.fetchurl {
    url = "ftp://host/path/${params.file}";  # explicit FTPS: ftp:// with
    curlOptsList = [ "--ssl-reqd" ];           # --ssl-reqd (often faster than https)
    hash = params.hash;                        # "sha256-..." (nix hash file <file>)
  };
  data = pkgs.runCommand "data.fits" { } "gunzip -c ${raw} > $out";
in
{
  run = sdk.pythonCard (common // {
    src = pkgs.runCommand "load-src" { } ''
      cp -r ${./src} $out
      chmod -R +w $out
      echo 'EVENTS = "${data}"' > $out/observation.py
    '';
  });
  api = sdk.pythonApi common;
}
```

The Card returns the store location as data in a FlatBuffer table such as
`{ path:string; }`. A long build step (a simulation) can use the same approach
with `pkgs.runCommand` or `stdenv.mkDerivation`.

### Path outputs

Declare the output as `string (tsugumi: "path")` and create the file or directory
under the Card's working directory before returning its path. See [Building outputs](#building-outputs).

### Reading a merge output

The input is a table whose `value` is a union with one member per merge input; the union's type
says which port the value arrived on. In the SDKs that use FlatBuffers' object API (C++, Python,
Go, Rust, C#), read the generated union type and value fields; R, Julia and Ruby give
`{type = "port", value = ...}`. Each SDK's guide shows the generated names.

### Templates

A template is a folder of Card definition directories: one Card, several Cards, or part of a graph
with its internal connections and an optional `layout.bin` fragment. Choose **From Template…** in
**New Card** to copy one into the graph. The copy gets fresh Card IDs; connect its boundary ports to
the rest of the project.

| Store | Where | Scope |
| --- | --- | --- |
| **Project** | The project's `templates/` folder | Versioned with the project |
| **Host** | `templates/` in this host's Tsugumi support folder | Shared by the projects on this host |

The menu lists both stores separately, with nothing preselected; the same name in both stores
appears twice with its store label. Add a template to either store from a folder or from a public
HTTPS Git repository with a ref (a commit ID selects an exact version) and the template's
subdirectory. The examples in [tsugumi-kit's templates](https://github.com/gen740/tsugumi-kit/tree/main/templates)
import with the repository `https://github.com/gen740/tsugumi-kit.git` and a subdirectory such as
`templates/filter-counts`. Tsugumi lists, imports and copies templates; a person or the Agent
chooses one, and there is no default per language. The Agent may list and copy templates; adding
or importing one needs a person's approval. The [template format](https://github.com/gen740/tsugumi-kit/blob/main/docs/templates.md)
is described in the kit.

### Draft wire ends and implementation

A Draft (`_card.json` with `"type": "draft"`) has no input or output ports,
including when `_outputs.fbs` exists.
A connection attaches directly to the Draft with `{"card":"plan","port":""}`.
The `port` field is required; the empty string is reserved for Draft ends.
Any number of wires can enter or leave a Draft, including wires between Drafts.
A real Card's endpoint still names an existing input or output. The other end's
port name and the Draft's content explain the planned connection.

Draft ends carry the Empty type, do not constrain type checking, and never
prevent saving because of their type. Drafts and Cards requiring their results
do not run; independent implemented paths run as before.

When implementing a Draft, explicitly rebind **every** incoming wire to a real
input and every outgoing wire to a real output in the **same save** as the new
definition. Include `SetProject` in `project_save`; inside a Card in Card, update
its project's `tsugumi.json` and any boundary endpoints too. Ends on other Drafts
stay empty. A single possible port is still an explicit choice. A save leaving
a wire on a non-existent port of a real Card fails with `validation_failed`
and names the wire. No endpoint is inferred or migrated.

In Mac and Web Card settings, choose **Implement Draft by hand** under General,
edit the definition's inputs, sources and output schema, and fill **Rebind wires**
under Ports before Apply. Each wire starts with an empty choice. Apply saves the
implementation and the selected endpoints together; a rejection leaves the
working copy open for correction.

## What to read next

| To know | Document |
| --- | --- |
| Branches, merges and loops, passing files, data pinned with Nix | [The bundled demos](Demos.md) (their code is still to be ported to the current Card ABI: [TODO](Demos.md#todo-port-the-demos-to-the-current-card-abi)) |
| Examples of code that runs | [tsugumi-kit's templates](https://github.com/gen740/tsugumi-kit/tree/main/templates) |
| One language's API, dependencies and threads | [tsugumi-kit](https://github.com/gen740/tsugumi-kit/tree/main) |
| The contract between the core and an SDK | [Card ABI](CardABI.md) |
| Leaving implementation to the Agent | [the Agent in Features](features.md#9-agent) |
| What the words mean | [Glossary](Glossary.md) |
