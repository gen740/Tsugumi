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
  "tsugumiVersion": "0.9.4",
  "name": "Temperature conversion",
  "nixPath": [
    {
      "name": "nixpkgs",
      "url": "github:NixOS/nixpkgs/nixos-unstable"
    },
    {
      "name": "tsugumi-kit",
      "url": "github:gen740/tsugumi-kit/main"
    }
  ],
  "trackedPaths": [],
  "agentExclude": [
    ".git",
    ".DS_Store"
  ],
  "recordLogs": true,
  "recordIO": false,
  "logLimitBytes": 134217728,
  "outputLimitBytes": 68719476736,
  "outputLimitEntries": 1000,
  "canvas": {
    "cards": [
      {
        "id": "start"
      },
      {
        "id": "settings"
      },
      {
        "id": "convert"
      }
    ],
    "connections": [
      {
        "from": {
          "card": "start",
          "port": "out"
        },
        "to": {
          "card": "settings",
          "port": "start"
        }
      },
      {
        "from": {
          "card": "settings",
          "port": "temperature"
        },
        "to": {
          "card": "convert",
          "port": "celsius"
        }
      }
    ]
  }
}
```

Use the version of Tsugumi you run as `tsugumiVersion`. See [Project files](Reference.md) for the complete format.

The lock below is an example of the pins for these declarations. Use an explicit update to resolve new pins for your own declarations. [Project Nix search path](features.md#project-nix-search-path)

`tsugumi.lock`

```json
{
  "entries": [
    {
      "name": "nixpkgs",
      "url": "github:NixOS/nixpkgs/nixos-unstable",
      "rev": "1c89083e451e891152365b799595cf7790a43325",
      "narHash": "sha256-Y1CiZ4SoBUjLQyQM/pY+rS2dXD3CHLPstF0lmOc73D4="
    },
    {
      "name": "tsugumi-kit",
      "url": "github:gen740/tsugumi-kit/main",
      "rev": "2c56fee956c62950c8cf925a821d4bf5f97a8598",
      "narHash": "sha256-+QxSovos92wRFoAD4MlpTEkdJtoNJnXU1qplCsWJOws="
    }
  ]
}
```

`definitions/start/_card.json`

```json
{
  "type": "trigger",
  "symbol": "play.circle"
}
```

`definitions/start/README.md`

```md
---
title: start
description: Start a run.
---
```

### The input value Card

The `value` parameter has the same name as the field in the `Temperature` message. The `temperature` field of `Outputs` names the output port.

`definitions/settings/_card.json`

```json
{
  "type": "input",
  "symbol": "slider.horizontal.3",
  "inputs": [],
  "maxRuns": 1,
  "reuseWorker": false,
  "timeoutSeconds": 0,
  "buildTimeoutSeconds": 0,
  "recordLogs": "project",
  "recordIO": "project",
  "sandbox": {
    "filesystem": true,
    "pid": true,
    "ipc": true,
    "identity": true,
    "limits": {
      "cpu": 0,
      "memoryMiB": 0
    }
  },
  "grants": {
    "network": false,
    "gui": false
  },
  "system": "aarch64-linux",
  "workerCapabilities": []
}
```

`definitions/settings/README.md`

```md
---
title: settings
description: Supply the temperature in Celsius.
---
```

`definitions/settings/params.nix`

```nix
{
  value = 20.0;
}
```

`definitions/settings/_outputs.fbs`

```text
namespace thermometer;
table Temperature { value:double; }
table Outputs { temperature:Temperature (required); }
root_type Outputs;
```

`definitions/settings/default.nix`

```nix
{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/input> { inherit pkgs; };
in {
  run = sdk.input {
    outputs = ./_outputs.fbs;
    values.temperature.value = params.value;
  };
}
```

### The conversion Card

`definitions/convert/_card.json`

```json
{
  "type": "code",
  "symbol": "chevron.left.forwardslash.chevron.right",
  "inputs": [
    {
      "name": "celsius"
    }
  ],
  "maxRuns": 1,
  "reuseWorker": false,
  "timeoutSeconds": 0,
  "buildTimeoutSeconds": 0,
  "recordLogs": "project",
  "recordIO": "project",
  "sandbox": {
    "filesystem": true,
    "pid": true,
    "ipc": true,
    "identity": true,
    "limits": {
      "cpu": 0,
      "memoryMiB": 0
    }
  },
  "grants": {
    "network": false,
    "gui": false
  },
  "system": "aarch64-linux",
  "workerCapabilities": []
}
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
  scale = 1.8;
  offset = 32.0;
  unit = "°F";
  cxxflags = [ "-O2" ];
  plot = true;
}
```

`definitions/convert/default.nix`

```nix
{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/cpp> { inherit pkgs; };
  common = {
    inherit params;
    inputs = ./_inputs.fbs;
    outputs = ./_outputs.fbs;
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

`default.nix` uses `cxxflags` and `plot` to configure the build. The SDK embeds the evaluated parameters in the Card build. The SDK generates the interface from the port schema, `_outputs.fbs` and the names and types of the evaluated `params`.

`definitions/convert/README.md`

```markdown
---
title: convert
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
  const double fahrenheit = celsius.value * Card::params.scale + Card::params.offset;
  card::display::send(card::display::Scalar{fahrenheit, Card::params.unit});
  Card::Output output;
  output.fahrenheit.emplace().value = fahrenheit;
  return output;
}
```

`Card::InputType` selects the connected type or a fallback that lets the source compile while unwired. `input.get` reports `input_missing` if the call requires an absent value. In the Card editor's Source view, press **Build SDK** to read the generated names and declarations.

## Running it and checking

1. Press **Run** (⌘R). The first build prepares the Card's environment.
2. Open `convert`'s live view to see `68 °F`. Use **Run History** to inspect the output on `fahrenheit` and the logs for evaluation, build and execution.
3. Change the `offset` to `0.0` and choose **Run Only This Card (Last Inputs)**. The editor saves the new literal in `params.nix`. This experiment uses the saved 20-degree input and yields 36 after rebuilding the Card. Set the value back to `32.0` to restore the original calculation.

## Inputs and outputs

Name each input in `_card.json` and connect at most one wire. An output can
feed several inputs. Use Merge to join several sources. A normal Card waits
for every connected input and skips if an upstream Card finishes without it.

The root table of `_outputs.fbs` lists the output ports. Each field names a
table type. Mark outputs the Card must produce as `(required)`. A Card may
omit other outputs to select which downstream Cards run.

Tsugumi generates `_inputs.fbs` from the wiring and the upstream schemas. Use
**Build SDK** to read the generated interface. Merge candidates appear as a
union in that schema when their types differ. The SDK decodes each arrival
using its schema ID.

### Building outputs

Pack the port table with the SDK. Report errors through its failure API.
Use `cancelled` and `remaining_ms` for work that observes cancellation and deadlines.

### Artifacts

Include `tsugumi/artifact.fbs` and use `tsugumi.Artifact` for a file or directory.
Use the SDK to create a writable directory or upload existing contents. Return
the Artifact in an output. A downstream Card can open the granted Artifact for
reading or forward its ID. The directory is sealed when the creating call ends.

## Views

Return `views` at the top level of `default.nix` alongside `run`. The example
uses the SDK's Markdown view. A custom declaration has `name`, `type` and `blob`.
The HTML file receives and sends bytes on a MessagePort. Display traffic is
live, and data saved for later inspection should go through output ports.

See [Views](CoreAndSDK.md#views) for the display protocol and Dawn contract.

## Worker state

With `reuseWorker: false`, each call starts with a fresh worker. Set `reuseWorker: true` to keep one worker for the Card: memory, loaded libraries and files in its working directory survive across calls and runs. One worker takes one call at a time. There is no hook at the start of a run, so a Card that keeps state decides from its inputs when to reset it, for example starting over when a loop's initial value arrives. Resident workers have no count, idle-time or memory eviction limit, including workers used for monitoring. They live until the project's environment stops or the Card's definition, SDK, grants or a secret value changes; a call already running when the definition changes finishes on the old version. A failed, cancelled or timed-out call always discards its worker; the next call starts fresh and logs the reason, as it does after a crash. Each invocation records whether its worker already held state (`worker_reused`). Ordinary fresh workers prepared for speed remain limited to 32, ten minutes idle and 2 GiB each.
A call granted a project artifact uses a fresh worker. A worker that creates
a project artifact retires when the call returns.

| Location | Use | Lifetime |
| --- | --- | --- |
| Working directory, also `HOME` | The Card's files and caches. New outputs are created here. | The worker. A reused worker keeps the contents. |
| `TMPDIR` | Scratch. Read the path from the environment. | Cleared for each call. |
| `/tmp` | On Linux workers, the shared VM and remote workers, the same scratch as `TMPDIR`. | On native macOS only `TMPDIR` is scratch. |

The worker sets the working directory, `HOME`, `TMPDIR` and the environment at launch. An opened artifact's path is valid only during its call. A cache in `HOME` should be safe to rebuild when a later call finds it missing. The Card's process gets no shell and no `PATH` of its own: there is no implicit `/bin/sh`. A Card that runs other programs declares them to its SDK's builder, for example `sdk.pythonCard (common // { src = ./src; tools = [ pkgs.coreutils pkgs.ripgrep ]; })`, and the builder puts exactly those on the Card's `PATH`. Packages come from the project's pinned nixpkgs.

## Card definition reference

### A Card's folder

| File | Role |
| --- | --- |
| `_card.json` | Card type, input names and execution settings |
| `_outputs.fbs` | Output ports and their types |
| `_inputs.fbs` | Generated input schema for connected ports |
| `params.nix` | Current parameter literals; absent when the Card has no params |
| `default.nix` | Nix definition receiving `{ params, inputs }` |
| `README.md` | Required `title` and `description` front matter and an optional Markdown body |
| `_comments.json` | Review comments |
| `src/`, `views/` | Card source and optional custom view source |

The folder id identifies the Card in the project. The README's `title` is the name on the canvas.

### Card types

Use Code for processing, Input for values built with Nix, and Trigger to start
the workflow. Merge and Collect route and aggregate arrivals. Draft is a
placeholder, and Subgraph embeds a project from `externals/<id>/`.

### An input Card with fixed values

The `sdk.input` helper takes the output schema and a map of output port names
to FlatBuffers JSON table values. It builds the port bytes, which core emits.
See the [temperature Card](#the-input-value-card) above.

### An input Card that passes on a project file or folder

Use `sdk.artifact ./data.csv` where a value of type `tsugumi.Artifact` is
expected in `sdk.input`. The SDK packages the file or directory in the build
output. Core registers its existing read-only store directory.

### Parameters

`params.nix` is a flat attrset of literals. A Card without params has no
`params.nix`; a missing file is the same as `{ }`. Entries can be booleans, signed
64-bit integers, doubles, strings and nonempty homogeneous lists of these types.
`10` and `10.0` have different types. Editing a value in the UI replaces that
literal in place, preserving the type, other entries and comments. Pass
the evaluated values to the builder. A derivation depends on the values
its expression uses.

### Code Card settings

A Code Card has `"type": "code"`. Declare input port names in `inputs`. The connected outputs supply the input types, and `default.nix` selects the SDK.

Set `grants.network` to true to allow network access. TLS uses the Nix `cacert` bundle through `SSL_CERT_FILE`; report downloaded data's URL, size and SHA-256. For immutable public data, prefer a pinned Nix fetch as described below.

Write the target Nix system in `system` and any required worker capability bits in `workerCapabilities`. See [Worker selection](RemoteExecution.md#worker-selection). Write all four isolation flags and both resource limits in `sandbox`. Use zero for an unlimited CPU or memory budget. A Linux worker with delegated cgroup v2 applies nonzero limits to the Card's process.

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

A definition can return several variants. An Input Card can map a list of literals
to distinct values using the same mechanism.

```nix
run = map (value: {
  label = toString value;
  run = sdk.input { outputs = ./_outputs.fbs; values.temperature = { inherit value; }; };
}) params.temperatures;
```

Variants from different Cards form batch axes. Collect turns a batch into one
`Collected` envelope list. Runtime port values flow between Cards independently
of their derivation inputs.

Set `exports` to publish Nix values to guaranteed downstream Cards. A downstream
definition reads them as `inputs.<card_id>.<attr>`. For example, C can see both
A and B in `A → B → C`. TODO(open): Exports across Merge, Collect, feedback and Drafts. A subgraph upstream contributes no `inputs` key for the subgraph or its inner producer and does not pass through its own upstreams. Cards inside a child project likewise receive no parent `inputs` keys.

### Data fixed by Nix

Use a locked project source and include its store path in the Card build when
the program needs immutable data. To send a project file through the graph,
declare an Artifact output and publish it with an Input Card.

### Reading a merge output

Build SDK after connecting the Merge. Decode a single candidate as its
ordinary type, or select a member of the generated union when there are
multiple candidates. Merge forwards the original bytes and artifact IDs.

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
| Conditional outputs, loops, artifacts and data pinned with Nix | [The bundled demos](Demos.md) ([display port status](Demos.md#demo-status)) |
| Examples of code that runs | [tsugumi-kit's templates](https://github.com/gen740/tsugumi-kit/tree/main/templates) |
| One language's API, dependencies and threads | [tsugumi-kit](https://github.com/gen740/tsugumi-kit/tree/main) |
| The contract between the core and an SDK | [Card ABI](CardABI.md) |
| Leaving implementation to the Agent | [the Agent in Features](features.md#9-agent) |
| What the words mean | [Glossary](Glossary.md) |
