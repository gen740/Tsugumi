# tsugumi-pack

Export a Tsugumi Project as a standalone Apple Silicon app for macOS 26 or
later. Nix is used on the exporter's Mac to build the Cards.

```sh
TSUGUMI_CODESIGN_IDENTITY="Apple Development" pack/build.sh
demos_dir=$(nix build --no-link --print-out-paths .#demos)
dist/tsugumi-pack/bin/tsugumi-pack "$demos_dir/en/glowroot" --output Glowroot.app
open Glowroot.app
```

The exported app runs its Workflow with the definitions and parameter values
fixed at export. The trigger can be selected with `--start` when the Project
has multiple triggers. A PANEL view opens in the app's main window. Separate
windows and full-screen views use the same Dawn and HTML hosts as the macOS
Tsugumi app.

## Export

The exporter stages the Project without its Git repository or local Tsugumi
history. `tsugumid --prepare-native-cards` resolves the graph and validates
the schemas, port names and types, including nested Cards. Every executable
Card must declare `aarch64-darwin` in `systems`. Exports build every Card,
including branches that may never run, from the Project's pinned Nix sources.
A build error names the Card.

The exporter copies the library and runtime closure, bundles view blobs, and
rewrites Mach-O install names and their dependency references. Separate
Card libraries get distinct install names. The exporter signs the nested
code and app with the selected identity. An Apple Development identity
supports local testing. Distribution outside the development team requires
Developer ID signing and notarization.

## Execution modes

- **Default.** Cards run in local processes under the Darwin worker's
  Seatbelt rules. The runtime honors `reuseWorker`. The host lends a shared
  result mapping, then passes read-only spans into that mapping to downstream
  workers. The mapping lives as long as a reader holds it.
- **Fast (`--fast`).** All Card libraries are loaded together at startup
  with `RTLD_LOCAL` and stay resident. Each invocation calls the library
  directly and borrows input spans from producer results. Independent branches
  can run concurrently. Fast export shares a bundled copy of each declared
  language runtime and rejects conflicting versions and multiple Go
  libraries. Fast Cards share the process's privileges and address space.
  Cancellation is cooperative. An invocation that runs past its hard deadline
  terminates the packed app with a diagnostic.

Packed execution trusts its bundled Cards to hand off results when they
return. The app author fixes and ships those implementations together. The
development worker still uses its ordinary result verification and artifact
sealing rules. Both packed modes enforce the 16 MiB message limit and required
outputs. Debug exports additionally verify result FlatBuffers.

Each declared host path maps to a directory of the same name under the
app's Application Support folder. Run history and kept artifacts are stored
in the app's `.tsugumi` directory there. A release export records run
metadata and kept artifacts. A debug export also records input and output
values for inspection. Artifacts created without the keep flag expire when
their run ends.

## Debug exports

Pass `--debug` at export to enable a local JSON RPC socket. The launcher
prints the socket path. To choose it, pass `--debug-socket /absolute/path`
to the app. The containing directory must be owned by the current user with
mode `0700`. The socket uses mode `0600` and checks the peer UID.

The endpoint accepts newline-delimited JSON RPC frames:

- `ViewWindowsRequest` lists sessions, including whether a Dawn view
  requested audio. `ViewActionRequest` sends pointer, key, text or gamepad
  input and captures snapshots.
- `PackRunRequest` starts at a Card path and accepts typed inputs as
  `{name, schema, bytes}` entries. The schema and payload must match the
  declared input port.
- `RunStatusRequest`, `RunInvocationsRequest`, `RunLatestInvocationsRequest`,
  `RunRecordRequest` and `RunValueReadRequest` inspect runs and values.
- With `--test-clock`, `StepClockRequest` advances the Dawn presentation
  clock by the requested number of milliseconds and reports `elapsed_ms`.
  In this mode replay uses the manual clock for rendering and input. Audio
  devices and gamepads are driven by the real clock in ordinary exports.
  `ViewWindowsRequest` reports the Dawn module's `audio_requested` capability
  in either debug configuration. Cards read their own clocks.
- `RunCancelRequest` cancels a run. `PackQuitRequest` ends the debug app.

A release export has no debug socket. The packed app's presentation uses
a separate private connection to its own bundled runtime.
