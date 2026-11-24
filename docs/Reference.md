# Project files

Tsugumi reads each project from its own `tsugumi.json`. When a required field is missing or a Card definition contains a field outside its type's set, loading fails with `invalid_document` and identifies the file and field. Creation writes the settings explicitly.

## `tsugumi.json`

| Field | Meaning |
| --- | --- |
| `tsugumiVersion` | The Tsugumi version that created or last saved the project. |
| `name` | The project name. |
| `nixPath` | A list of `{name, url}` declarations for Nix search paths. The matching pins live in `tsugumi.lock`. |
| `trackedPaths` | Extra literal paths relative to the project root to keep in Tsugumi's own project versions. This setting does not change Git tracking. |
| `agentExclude` | Files and directories hidden from the Agent. Project creation writes the host's project default when it uses a host. |
| `recordLogs` | Whether to retain invocation stdout and stderr. |
| `recordIO` | Whether to retain structured invocation inputs and outputs. |
| `logLimitBytes` | Combined retained stdout and stderr budget per invocation. |
| `outputLimitBytes` | Byte budget for an invocation's output directory. |
| `outputLimitEntries` | Entry budget for an invocation's output directory. |
| `canvas` | The `cards` and `connections` arrays. A Card entry has `id`. Each connection has `from` and `to` endpoints, each with `card` and `port`. |
| `boundary` | Optional, and only for a child project. It requires both `inputs` and `outputs` arrays, which may be empty. An entry has an outer port `name` and an inner endpoint `card` and `port`. |

Every field except `boundary` is required. Tsugumi refuses to load a root or child project whose `tsugumiVersion` differs from its own version. The error reports both versions. Before v1.0, Tsugumi supplies no project migration. Saving writes the current version.

## Card ids and port names

Card ids, input and output port names, boundary port names and child project directory names must match `[a-z][a-z0-9_]{0,63}`. The rule prevents case collisions on case-insensitive APFS volumes and avoids hidden directories beginning with a dot. It also keeps identifiers usable in generated FlatBuffers namespaces and as Nix attribute names. Under `_outputs.fbs`, the fields of `Outputs` name the output ports. A wire can attach to a Draft itself using the empty `port` string as a marker for an end without a named port.

## `_card.json`

Each `definitions/<card id>/_card.json` uses the fields for its type. **R** means required, **O** means optional, and **—** means the field is forbidden.

| Field | Code | Input | Merge | Collect | Draft | Trigger | Subgraph |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `type` | R | R | R | R | R | R | R |
| `symbol` | R | R | R | R | R | R | R |
| `inputs` | R | R | R (at least one) | — | — | — | — |
| `maxRuns` | R | R | R | R | — | — | — |
| `reuseWorker` | R | R | — | — | — | — | — |
| `timeoutSeconds` | R | R | — | — | — | — | — |
| `buildTimeoutSeconds` | R | R | — | — | — | — | — |
| `recordLogs` | R | R | — | — | — | — | — |
| `recordIO` | R | R | R | R | — | — | — |
| `sandbox` and `sandbox.limits` | R | R | — | — | — | — | — |
| `grants.network` and `grants.gui` | R | R | — | — | — | — | — |
| `grants.hostPaths` | O | O | — | — | — | — | — |
| `system` | R | R | — | — | — | — | — |
| `workerCapabilities` | R | R | — | — | — | — | — |
| `secrets` | O | O | — | — | — | — | — |

- `type` is the lowercase kind name. `symbol` is the icon's symbol name. `inputs` is an array of objects with a `name` field. `maxRuns` bounds activations per run. `reuseWorker` lets the worker keep state across invocations and runs.
- `timeoutSeconds` limits a Card call and `buildTimeoutSeconds` limits the build. A positive value is in seconds. Zero means no limit.
- `recordLogs` and `recordIO` use `"project"` to inherit the owning project's value, `"on"` to enable recording, or `"off"` to disable it.
- `sandbox` contains the four booleans `filesystem`, `pid`, `ipc` and `identity`. Its `limits` object declares `cpu` in cores (0 through 256, fractional values allowed) and `memoryMiB` in MiB (0 through 1048576). Zero means no limit for either resource.
- `grants` contains the booleans `network` and `gui`. An optional `hostPaths` array lists Mac folders. Every entry requires an absolute `path`, a `name` under `/host/`, and a boolean `write`. Use `false` for read-only access.
- `system` explicitly names the Linux Nix system for the build and runtime, for example `"aarch64-linux"` or `"x86_64-linux"`. A worker with a different native system needs a matching emulator.
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

## Nix `inputs` across child project boundaries

`inputs.<card_id>` exposes exports from upstream Cards at the same project level. A subgraph upstream contributes no key for its own id or the inner producer's id. Its upstreams do not pass through it, and Cards inside a child project receive no Nix inputs from the parent. Wires across project boundaries still carry runtime data.
