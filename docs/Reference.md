# Project files

Tsugumi reads each project from its own `tsugumi.json`. When a required field is missing or a Card definition contains a field outside its type's set, loading fails with `invalid_document` and identifies the file and field. Creation writes the settings explicitly.

## `tsugumi.json`

| Field | Meaning |
| --- | --- |
| `formatVersion` | The project file format, an integer. It changes only when the format of the project files changes, not with each Tsugumi release. The current format is `1`. |
| `name` | The project name. |
| `nixPath` | A list of `{name, url}` declarations for Nix search paths. The matching pins live in `tsugumi.lock`. |
| `trackedPaths` | Extra literal paths relative to the project root to keep in Tsugumi's own project versions. This setting does not change Git tracking. |
| `agentExclude` | Files and directories hidden from the Agent. Project creation writes the host's project default when it uses a host. |
| `recordLogs` | Whether to retain invocation stdout and stderr. |
| `recordIO` | Whether to retain structured invocation inputs and outputs. |
| `logLimitBytes` | Combined retained stdout and stderr budget per invocation. |
| `outputLimitBytes` | Byte budget for an invocation's output directory. |
| `outputLimitEntries` | Entry budget for an invocation's output directory. |
| `canvas` | The `cards` and `connections` arrays. A Card entry has `id`. Each connection has `from` and `to` endpoints, each with `card` and `port`. `port` is absent on a Draft end and required on every other end. |
| `boundary` | Optional, and only for a child project. It requires both `inputs` and `outputs` arrays, which may be empty. An entry has an outer port `name` and an inner endpoint `card` and `port`. A Draft has no ports, so it cannot be on a boundary. |

Every field except `boundary` is required. Tsugumi refuses to load a root or child project whose `formatVersion` differs from the format it reads. The error reports both formats. Before v1.0, Tsugumi supplies no project migration.

The check is on the file format, not on the Tsugumi version. Tsugumi is designed so that the same project gives the same processing on any version, so the version that ran a project does not matter; only the format of its files does.

## Card ids and port names

Card ids, input and output port names, boundary port names and child project directory names must match `[a-z][a-z0-9_]{0,63}`. The rule prevents case collisions on case-insensitive APFS volumes and avoids hidden directories beginning with a dot. It also keeps identifiers usable in generated FlatBuffers namespaces and as Nix attribute names. Under `_outputs.fbs`, the fields of `Outputs` name the output ports. A Draft has no ports: a wire end on a Draft names only the `card`, and writing a `port` there is a load error, as is leaving it out anywhere else.

## `_card.json`

Each `definitions/<card id>/_card.json` uses the fields for its type. **R** means required, **O** means optional, and **—** means the field is forbidden.

| Field | Code | Merge | Collect | Draft | Trigger | Subgraph |
| --- | --- | --- | --- | --- | --- | --- |
| `type` | R | R | R | R | R | R |
| `symbol` | R | R | R | R | R | R |
| `inputs` | R | R (at least one) | — | — | — | — |
| `maxRuns` | R | R | R | — | — | — |
| `reuseWorker` | R | — | — | — | — | — |
| `timeoutSeconds` | R | — | — | — | — | — |
| `buildTimeoutSeconds` | R | — | — | — | — | — |
| `recordLogs` | R | — | — | — | — | — |
| `recordIO` | R | R | R | — | — | — |
| `sandbox` and `sandbox.limits` | R | — | — | — | — | — |
| `grants.network` and `grants.gui` | R | — | — | — | — | — |
| `grants.hostPaths` | O | — | — | — | — | — |
| `systems` | R | — | — | — | — | — |
| `workerCapabilities` | R | — | — | — | — | — |
| `secrets` | O | — | — | — | — | — |

- `type` is the lowercase kind name. `symbol` is the icon's symbol name, or `"none"` for the UI's own icon for the kind. Tsugumi writes `"none"` when it creates a Card. `inputs` is an array of objects with a `name` field. `maxRuns` bounds activations per run. `reuseWorker` lets the worker keep state across invocations and runs.
- `timeoutSeconds` limits a Card call and `buildTimeoutSeconds` limits the build. A positive value is in seconds. Zero means no limit.
- `recordLogs` and `recordIO` use `"project"` to inherit the owning project's value, `"on"` to enable recording, or `"off"` to disable it.
- `sandbox` contains the four booleans `filesystem`, `pid`, `ipc` and `identity`. Its `limits` object declares `cpu` in cores (0 through 256, fractional values allowed) and `memoryMiB` in MiB (0 through 1048576). Zero means no limit for either resource.
- `grants` contains the booleans `network` and `gui`. An optional `hostPaths` array lists grants. Each entry requires a `name` declared in the containing project and a boolean `write`. Use `false` for read-only access. The optional project `hostPaths` array contains `name` and `description`; host settings store the folder binding.
- `systems` lists the Nix systems the Card works on, at least one and without duplicates: `"aarch64-linux"`, `"x86_64-linux"`, `"aarch64-darwin"` or `"x86_64-darwin"`. A worker whose native system is in the list builds and runs the Card natively. Any other worker builds and runs the first entry, which needs a matching emulator. Which worker runs the Card does not depend on this list.
- `workerCapabilities` lists distinct integer bit numbers from 0 through 63. An empty list requests no bits. Every bit must be present among the worker's bits declared in host settings. See [Worker selection](RemoteExecution.md#worker-selection).
- `secrets` lists the names of credentials provided to the process by hostd as environment variables. The project stores the names only.

## Child projects

A subgraph Card with id `<id>` embeds the project at `externals/<id>/`. A nested subgraph repeats this structure inside its owning project's directory. Each subgraph Card owns its own child project. Renaming the Card moves the child project directory in the same transaction. Importing a project writes both `definitions/<id>/` and `externals/<id>/` together.

The following are `invalid_document` load errors at every project level.

1. A Card on the canvas lacks its definition directory, or a definition directory has no corresponding Card on the canvas.
2. A subgraph Card lacks `externals/<id>/tsugumi.json`.
3. A directory under `externals/` has no corresponding subgraph Card at that level.

## `README.md`

Each Card definition requires a `README.md` beginning with front matter containing exactly `title` and `description`, one line each. `title` is the non-empty display name in Mac and Web. If the README cannot be read, the UI displays the Card id. `description` may be empty. Write a space after its colon, including when its value is empty. A Markdown body can follow the closing fence.

## Nix `inputs` across Cards

`inputs.<card_id>` holds the `exports` of an upstream Card at the same project level, and what that Card itself sees passes on downstream. Built-in Cards pass exports on as follows.

- **Subgraph**: a downstream Card sees `inputs.<subgraph Card id>`, which holds the exports of the inner Cards wired to the subgraph's boundary outputs. When several inner Cards feed boundary outputs, their attribute sets are merged, and the same attribute name from two of them is an evaluation error. Inner Card ids never appear outside, and Cards inside a child project receive no Nix inputs from the parent. Wires across project boundaries still carry runtime data.
- **Merge** exposes only the upstream exports common to all of its inputs.
- **Collect** exposes the exports of the Card it collects from.
- A **feedback wire** exposes nothing, so a Card never depends on its own exports.
- A **Draft** exposes nothing.
