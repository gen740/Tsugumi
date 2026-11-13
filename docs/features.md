# Tsugumi's features

Tsugumi connects small, typed steps called Cards into a Workflow. Run the whole graph, change one step, and inspect the recorded conditions and results. Use the macOS app or open the workbench in a browser with the `tsugumi` command. [Installation](INSTALL.md) · [First steps in the workbench](Usage.md#the-first-five-minutes)

This reference follows `main` and covers the 0.7 pre-release series. If you are using the 0.6.2 stable release, see its versioned documentation for the available features. [0.6.2 documentation](https://github.com/gen740/Tsugumi/tree/v0.6.2/docs)

## Find a feature

### Build and navigate a Workflow

- **Sketch before writing code.** Add a Draft with `e` while the Canvas has focus, describe a step, and draw its wires. Add a Start, Input, Switch, Fork, Branch, Merge or Collect Card when you already know how data should flow. [Canvas tools](Usage.md#canvas-tools) · [Kinds of Card](#kinds-of-card)
- **Organise a larger graph.** Right-click the Canvas and choose **Arrange** to place Cards or **Untangle Wires** to reduce crossings while keeping their positions. Group related Cards with **Group Selection**, or open a Card in Card to see its child Workflow and boundary ports. Import another Workflow as a Card in Card with the import button by **Add Card**. [Canvas tools](Usage.md#canvas-tools) · [Cards and Workflows](#2-cards-and-workflows)
- **Sweep inputs and implementations.** Give a Card a list of implementations in `run`, or send it a batch, and it runs every combination. Connect the batch to a Collect Card to pass one vector or Directory on. [Run lists and Collect](#run-lists-and-collect)
- **See references between Card definitions.** Turn on **Nix references (as written)**, then hover a Card or open the list. Follow `inputs.data` to the Card connected to the `data` input. [Nix references](Usage.md#canvas-tools)

### Run, inspect and compare

- **Try one change.** Run the Workflow once, change a Card parameter, then choose **Run from Here Downstream**. Use **Run Only This Card (Last Inputs)** to try the step by itself with inputs saved from a previous run. [Run from a Card](Usage.md#run-from-a-card)
- **See what happened.** Use the bottom timeline or open **Card settings → Run History**. Pick an invocation and open **Ports**, **Log**, **Packages** or **Info** to see its inputs and outputs, its logs, and the stage at which it failed or stopped. [Inspect an invocation](RunHistory.md#inspect-an-invocation)
- **Compare results and check the code that produced them.** Select two runs in **Compare Runs** to compare their recorded parameters, values and displays. Inspect the code and settings diff before choosing whether to revisit a saved version. [Compare two runs](RunHistory.md#compare-two-runs) · [Version history](Versions.md#the-version-tree)
- **Go back to an earlier version.** Open **Version History**, pick a version in the tree and choose **Start from This Version**. [The version tree](Versions.md#the-version-tree)
- **Check what an invocation ran with.** Open **Packages** in its run record and search the recorded runtime store paths. Open an evidence link from the Agent's chat to inspect a cited run, log or artifact. [Runtime packages](RunHistory.md#find-the-runtime-packages) · [Evidence links](RunHistory.md#follow-evidence-from-chat)

### Display results and keep files

- **Plot data with editable code.** Choose **Add Card → Visualization** and select Matplotlib, Plotly or Altair. For example, plot a numeric column as a histogram and inspect the actual bins kept with the figure. [Visualisation templates](Visualization.md)
- **Watch a running figure or show a value.** Try the Live Stream demo and select **Live (follow while running)** under **Card settings → General → Chart updates** to follow incoming revisions. Use a Bash Card to log a typed input and show it as text or an explicitly selected Markdown string, as the `result` Card of the marimo demo does. [Chart updates](Usage.md#chart-updates) · [Bash displays](CardGuide.md#bash)
- **Run a notebook or render a report.** Add a marimo notebook to run committed cells once and return typed results. Use an R Markdown or Quarto Card to render a parameterised HTML report from an upstream value. [marimo notebooks](Marimo.md) · [R Markdown and Quarto](Reports.md)
- **Keep a result for later.** In **Ports**, keep a File or Directory output as an artifact. Open its saved files under **Artifacts**, including a static preview for HTML. [Keep and open artifacts](RunHistory.md#keep-and-open-artifacts)

### Write Cards and pin environments

- **Choose a language.** **Add Card** offers a processing Card in Python, C++, Go, Rust, Ruby or C#. For Bash, R or Julia, set `language` in the Card's `_card.json`. The Card guide has the same typed input/output example in all nine languages. [First Card](CardGuide.md) · [Ruby](CardGuide.md#the-same-card-in-ruby) · [Go](CardGuide.md#go-cards) · [Rust](CardGuide.md#rust-cards) · [C#](CSharp.md)
- **Pin dependencies and control state.** Use **Project settings → Name and Environment** to pin Nixpkgs or add a trusted flake. Declare a Card-local lock where the SDK supports one (Python, Ruby, Go, Rust and C#). For a Card that needs interpreter or library state, turn on **Keep the worker's state** (`reuseWorker`) in its settings. [Nix environments](#3-reproducible-environments-with-nix) · [Language SDKs](#language-sdks) · [Resident workers](#resident-workers)
- **Use a shell or a signed build cache.** Open the project Terminal to run a tool such as `nix --version`. On Mac, configure a shared signed cache in **Settings → Binary Caches** and apply it on the next VM start. [Project terminal](Usage.md#project-terminal) · [Binary caches](BinaryCaches.md) · [Browser and CLI platforms](WebUI.md#the-tsugumi-command)
- **Run a Card on another Linux machine.** Start an execution daemon there, register it in **Settings → Execution service**, and choose it under **Execution service** in the Card's settings. [Run a Card on an execution daemon](RemoteExecution.md) · [Execution service](#execution-service)
- **Control a Card's access.** Use **Card settings → General** to review **Restrict file access**, and the network and host folder permissions under **Experimental**, before running code from another project. [Security and isolation](security.md#summary)

### Work with the Agent and explore examples

- **Turn a Draft into a Card.** Connect a model under **Settings → API**, describe an operation in a Draft and choose **Generate Cards**. Review the generated source, ports and run record. Use Card comments to request a specific change. [Generate from Drafts](#generating-cards-from-drafts) · [Generate from comments](#generating-cards-from-comments) · [Data and permissions](security.md#8-the-models-api-keys-agent)
- **Start from an example.** Browse Demos and open a copy to change a parameter, run it again, or follow the thrush's guide. Use the [documentation site](https://gen740.github.io/Tsugumi/)'s recordings to inspect example Workflows before installing. [Bundled demos](Demos.md)
- **Work from Claude Code.** Install the plugin to have Claude Code make and run Cards through Tsugumi's tools, with the work shown in the Web UI. [Claude Code plugin](claude-code.md)

## 1. Architecture

```text
macOS (host)                                       Shared Linux VM (one per app)
 SwiftUI UI ──┐                                     tsugumi-vmd (manages the VM)
 Agent ───────┼─ FlatBuffers RPC ─ 127.0.0.1 ─ vsock ─ project A: tsugumid ─ worker pool
 Other clients┘                                     project B: tsugumid ─ worker pool
                                                   nix-daemon ─ shared /nix/store
```

- **tsugumid (the core)**: one process per project. It manages and saves the document and handles the RPC, the scheduler, Nix evaluation, workers and the run history. Saving, running and reading history are all possible through the RPC alone, with no UI.
- **The UI and the Agent are equal clients**: both talk to the core with the same RPC contract, defined in `protocol/tsugumi.fbs`. The Agent does not imitate operations on screen; it sends RPC requests to the core directly. With the Agent disabled, every ordinary operation still works.
- **The shared VM**: one Linux VM started with macOS's `Virtualization.framework`, inside which each project's `tsugumid` runs with its own namespaces, uid and cgroup (details in [security.md](security.md)).
- **Web UI**: a browser workbench that uses the same project and RPC contract. With the Mac's `tsugumi` command the server runs on the Mac and its projects' `tsugumid` run in the same shared VM as the app; on Linux and when started from the repository, `tsugumid` runs directly on the host (Cards are isolated with Seatbelt on a Mac and bubblewrap on Linux; [Web UI](WebUI.md)).

## 2. Cards and Workflows

### Kinds of Card

| Kind | Role |
| --- | --- |
| Code Card | Defines its environment in `default.nix` and runs C++, Python, Bash, R, Julia, Ruby, Go, Rust or C# code |
| trigger | Where a Workflow's run starts (output port `out`) |
| input | Sends a value following the type in `_outputs.fbs`, edited directly on the Canvas (for a File type, the chosen file is copied into the run) |
| switch | Sends an editable Boolean on `value`, for example to the `condition` of a Branch |
| fork / branch / merge | Distribution, conditional branching and merging (a union type named by input port). A scalar Branch sends its value to `a` or `b`. A Branch that receives a batch sends the filtered results to both outputs, including empty batches. For bounded feedback, send a value back through a Merge and raise the Cards' `maxRuns` (default 1) as a safety cap and end the loop through a Branch; exceeding `maxRuns` fails the Run with `execution_limit`. When both `value` and `condition` are batches, a Branch runs every combination |
| Collect | Turns a batch into one value. Typed values become an `items` vector, and Files or Directories become a Directory |
| subgraph (Card in Card) | Handles a child Workflow as one Card. `boundary` maps the outer ports to Cards inside |
| Draft | An "unimplemented Card": write its name and content directly on the card, and connect it just by dropping a wire on it (its ports follow the wires; they are typed only if the Draft has an `_outputs.fbs`). The Agent can later generate its implementation |

### Typed ports

- An output port carries one wire. To send one value to several Cards it goes through a fork Card (so there is one way to write it, and the distribution is visible on the Canvas as a Card). An output with two or more wires is reported as `fan_out` and cannot run. Drawing another wire from an output already in use on the Canvas inserts a fork automatically (if the output is a fork or connects to one, that fork gets another branch).
- An output port's type is declared in a FlatBuffers schema (`_outputs.fbs`), and the type of each downstream input port is derived automatically from upstream. Structural compatibility is checked when wiring, and a merge builds a union type automatically.
- Values flow between Cards as FlatBuffers bytes. Automated tests check that 64-bit integers, enums, structs, unions and File / Directory pass between C++ / Python / bash (and R and Julia through an in-memory JSON adapter) without loss.

### Execution semantics (scheduler)

- A Code Card can wait for an arrival on all required inputs (`allInputs`) or trigger on each arrival (`eachInput`). An arrival is a single value or a batch of values. The setting is `trigger`, with the values `allInputs` or `eachInput`. A Merge consumes one arrival at a time.
- Bounded queues with backpressure, cancellation and an optional per-invocation deadline (`timeoutSeconds`; none by default for local runs). Each input port holds up to 16 unconsumed groups, and a run that can no longer progress fails with `execution_limit`. A Run can request concurrency from 1 to 64, with up to eight Code workers active at once. The iteration limit (`maxRuns`) counts how often the Card receives a set of inputs that triggers it.
- Downstream of an unselected scalar Branch output is `skipped`. A Card downstream of a failed, blocked or cancelled Card is `blocked`, with a reason. A Draft, and the Cards that need only its outputs, do not run and get no state; the canvas marks them as not run because of a Draft. Independent paths keep running.
- A run that was in progress when the process ended abnormally or restarted ends as `failed` with the failure kind `interrupted`.
- Partial runs with "Run from Here Downstream" or "Run Only This Card (Last Inputs)" reuse the inputs saved last time (including the contents of Files). When "Run from Here Downstream" reaches a merge, a Card that waits for all its inputs fills only the ports that receive nothing this time with saved inputs (an `eachInput` Card runs with only the values that arrived).

### Run lists and Collect

The [Signal detection sweep demo](Demos.md#signal-detection-sweep-signal-sweep) shows three inputs, two implementations and one collected result.

- A Code Card can supply a list of implementations in its Nix `run` attribute, such as `run = [ "echo first" "echo second" ];`. For each trigger, the runtime runs every combination of those implementations and the items arriving on its input ports. A single value or implementation contributes one choice. For example, two implementations and batches of three and four input values give 24 invocations. Nested implementation lists are invalid.
- Results follow the order of the input port names, the item order within each input batch and the implementation order in `run`. Parallel workers can finish in a different order. The Canvas uses thicker wires for batches and shows how many items they carry.
- An empty implementation list or input batch produces an empty output batch and starts no workers. Collect turns an empty batch into a typed empty vector or an empty Directory. With File or Directory inputs, Collect keeps each item in a numbered subdirectory, preserving its original name and any empty directories. Each entry in the Directory's ordered `items` has a `name` and relative `path`. Other File and Directory metadata remains in the producer's individual invocation records.
- There are at most 256 combinations per trigger, 4,096 worker invocations per Run and 32 MiB of serialized port values for all the inputs of one activation and for all its outputs. If one invocation fails, the runtime cancels its siblings and withholds the batch. The history keeps each invocation's outcome. A partial rerun with last inputs uses the whole saved batch, including saved Files and Directories.
- A collected value can be at most 16 MiB. Collect can copy up to 1,000 files and 64 GiB, with a total limit of 10,000 batch members and filesystem entries. Directory collection rejects symbolic links and special files.

### Graph previews on the canvas

- Plotly and Vega-Lite results appear as cached images on the canvas; Expand opens their full interactive viewer. The saved figure data is unchanged.
- Web previews are generated only for visible frames, one at a time in one sandboxed viewer. Completed previews use a bounded memory cache keyed by the full figure content and preview dimensions; closing or hiding a frame cancels its pending work, and an idle renderer is released. In the default `update` display mode a changed result keeps the previous picture, dimmed, until its own preview is ready; `live` mode keeps it at full strength.
- The shared macOS/Web viewer serializes and coalesces rapid updates, and finalizes replaced Vega views so their event listeners and timers do not accumulate.

## 3. Reproducible environments with Nix

### An environment per Card

```nix
{ pkgs, inputs, params }:
let
  sdk = inputs.tsugumi.sdk { languages = [ "cpp" ]; };
in
{
  run = inputs.tsugumi.cppCard { inherit sdk; src = ./src; };
  preRun = "mkdir -p scratch";
}
```

- `tsugumi:default` uses the Nixpkgs revision bundled with the installed version of Tsugumi. New projects are created already pinned to the app's revision. To keep using it after an app update, open **Project settings → Name and environment** and choose **Pin to This App's Nixpkgs**. You can also enter a revision and hash in the form `https://github.com/NixOS/nixpkgs/archive/<rev>.tar.gz#sha256-…`.
- Nix evaluation uses restricted eval and forbids IFD (Import From Derivation), and the URLs it may fetch during evaluation are limited to nixpkgs archives (build-time fetches must be pinned by hash).
- Builds reuse outputs already in the Nix store. A signed [binary cache](BinaryCaches.md) can supply a missing store path before Nix builds it locally.

### Sharing Nix exports through connections

- Set `exports` in the upstream Code Card's `default.nix`. If a downstream Code Card receives its output on the `data` port, that Card can use the exported attributes through `inputs.data`. The same exports are available to each implementation in a run list.
- The name after `inputs.` is the downstream input port name. The port must resolve to one Code Card that always runs first. `tsugumi` and `sources` are reserved for the SDK and configured sources.
- Fork, Collect, the value input of a Branch and Workflow boundaries preserve the Code producer's exports. A Merge can pass them through when all its inputs come from the same Code producer. A wire with several possible producers still carries its data, but a Nix reference to that input is ambiguous.
- Static diagnostics describe directly written references and flag unknown or ambiguous ports. Nix also checks references reached through an alias during evaluation.

- The [Canvas overlay](Usage.md#canvas-tools) draws the static references from this resolution. Nested targets meet their visible container and the list preserves the full paths. An invalid reference remains visible with its diagnostic. Saving a source refreshes the resolution.

### External Nix sources (`inputs.sources`)

- Flakes can be added under "External sources" in "Name and environment" of the project's settings. Enter an address (`github:owner/repo`, or the URL of a GitHub page) and press "Fetch" to see the commit and the list of packages for this environment, then press "Add". "Update" fetches a newer commit.
- Once added, it is kept in `tsugumi.json`, pinned by commit and hash. The only `type` for now is `flake`.

```json
"nixSources": [{ "name": "ciao", "type": "flake", "ref": "github:gen740/ciao/<commit>?narHash=sha256-..." }]
```

- `default.nix` can use the packages as they are. They are built when the Card runs.

```nix
{ pkgs, inputs, params }:
let ciao = inputs.sources.ciao.packages.ciao;
in { run = "${ciao}/bin/dmlist ..."; }
```

- `tsugumid` evaluates the flake with an evaluator separate from the Cards', and passes the Card only the packages' derivations. The Cards' evaluator can still fetch only nixpkgs and has no `builtins.getFlake`. The separate evaluator is the same pure evaluation as `nix build`, without IFD.
- A package is built with the nixpkgs of that flake's `flake.lock` (not the project's nixpkgs).
- A declaration applies only to that project's Cards (a Card in Card is another project). An external source whose name is not written in a `default.nix` is neither evaluated nor fetched (the check is a plain text match, so a mention in a comment counts).

### The built-in evaluator

- `tsugumid` embeds `libnixexpr` / `libnixstore` in its process and reuses the loading of nixpkgs, the evaluation state and the daemon connection across Cards and across runs. 
- On cancellation or the Card's `buildTimeoutSeconds`, the current Nix evaluation or build is interrupted; the evaluator keeps serving later calls.

### Keeping build results

- All projects share one `/nix/store`, and each project keeps its build results with its own GC roots (the last two generations per Card; ones unused for 30 days are collected automatically).
- A project's "Build Cache" pane (Delete) only removes that project's GC roots and build work files. Garbage collection of the shared store itself runs only from "Clean shared store" under "External Connections" in the app's settings. In the Mac shared VM, `keep-outputs = true` means compilers and the nixpkgs source stay after cleaning, so editing a Card does not download them again.

## 4. Card SDK and workers

### Stages

```text
SDK preparation → Nix evaluation → build → run (preRun → run → postRun) → output validation → saving
```

- An invocation records these six stages (`prepare`, `evaluate`, `build`, `execute`, `validate`, `save` in the RPC contract). A stage the Card does not need is skipped. The workbench adds "waiting for inputs" before them and "done" after them.
- Each stage's start and end times and its outcome (running, done, cached, reused, failed, cancelled) are recorded in `InvocationSummary.stages` and returned both in real-time notifications and in the history. `cached` is a Nix store hit, and `reused` is a build or worker kept from an earlier invocation of the same run. The UI draws the stage display and the highlighting of wires from this record alone, never guessing from the final state.
- `preRun`, `run` and `postRun` are steps of the one run stage. `preRun` prepares before the run, and `postRun` collects artifacts and cleans up. `postRun` also runs after `preRun` or `run` has failed, with `TSUGUMI_FAILED=1` and the ports already sent in `TSUGUMI_OUTPUTS`. After cancellation or a deadline, nothing further starts, `postRun` included.

### Language SDKs

- **C++**: with the generated header `card.hh`, port names become typed members as they are.
  ```cpp
  auto run(Card::Input& input) -> Card::Output {
    Card::Output out;
    out.next.emplace().value = input.number.value / 2;
    return out;
  }
  ```
  What does not depend on the port contract (visualisation, logs, the run context) is in the C++ runtime `libtsugumi_card.so`, so all that is built per C++ Card is the type definitions and a thin adapter.
- **Python.** Implement `main(ctx)` with the generated FlatBuffers classes, either through their accessors and a builder or in their object form (`NumberT`, `Pack`). Use a Card-local `uv.lock` and a pinned dependency output hash to build locked dependencies. [Python Card and dependency guide](CardGuide.md)
- **bash**: `run` is a string instead of a package. Inputs arrive per port, named with the port in upper case, as `JSON_<PORT>` (the path of a JSON file) / `FILE_<PORT>` / `DIR_<PORT>`. The JSON written to `$TSUGUMI_OUTPUT` is the output, type-checked per port, and the JSON written to `$TSUGUMI_DISPLAY` is what the Card shows.
- **Go.** Use `goCard` with generated typed inputs and outputs and managed goroutines. External modules are pinned with the Card's `go.sum` and the hash of its module cache. [Go Cards](CardGuide.md#go-cards)
- **Rust.** Use `rustCard` with generated typed inputs and outputs and lock crate dependencies with `Cargo.lock` and a vendor hash. [Rust Cards](CardGuide.md#rust-cards)
- **C#.** Use `csharpCard` with generated types and Tasks. The .NET runtime is hosted inside the worker process, and NuGet dependencies are pinned with the Card's `packages.lock.json` and the hash of its packages. [C# Cards](CSharp.md)
- **Ruby** (`rubyCard`): MRI runs inside the worker with typed Hash/Array values, exact 64-bit integers, live displays and optional resident state. The Card can name a Gemfile.lock and the hash of its gem files to pin its dependencies at build time. [The Card guide](../agent/Reference/card-guide.md#ruby-srcmainrb) describes the API.
- **R and Julia** (`rCard`, `juliaCard`): their interpreters run inside the worker. An invocation starts a new interpreter unless `reuseWorker` is on, which keeps the interpreter and its module state; displays publish during execution. How to write them is in [Your first Card](CardGuide.md).
- Ruby, R and Julia receive lists, Hashes and Dicts through an in-memory JSON conversion against each port's FlatBuffers schema, without JSON files.
- Every language other than bash is built into a `lib/main.so` that the one worker program (`tsugumi-worker`) loads and calls through the same Card ABI.

### Resident workers

- By default a Card keeps no state: `reuseWorker` is `false`, and every invocation gets a new worker (a new process, a new interpreter).
- A worker of a Card with `reuseWorker = true` stays resident across invocations and runs and keeps its state, static variables included (`onRunStart` / `on_run_start`, or `OnRunStartAsync` in C#, is called before the worker's first invocation of each run). Each invocation records whether its worker was reused.
- It ends automatically when its build conditions change, on failure or cancellation, after 10 minutes unused, above 2 GiB of memory, or when more than 32 are resident (the least recently used ends first).
- For a Card without `reuseWorker`, the new worker for next time is started ahead in the background (sandbox started, `.so` loaded, Python imports done, the Ruby, R or Julia source loaded) and kept waiting. At most 32 wait at once, and one unused for 10 minutes ends.

### Execution service

- Card settings show the **Execution service** for a Code Card. Set `"executionService": "local"` in `_card.json` to select the service of the connected project daemon explicitly. Omitting the field uses the same local service.
- The supported app and CLI installations run Cards on Linux. On macOS they use the shared Linux VM, and on Windows the command runs in WSL2. Running a macOS daemon directly is a development route with its own capabilities.
- The project daemon reports the local service and registered execution daemons. **CPU architecture** selects the Nix build and execution target within the selected service. A project keeps its saved service selection on installations where that service is unavailable. The selector shows the saved choice, and running stops with a diagnostic until the user registers that service or selects another one.
- To build and run a Code Card on another Linux machine, register its daemon in **Settings → Execution service** with a name, a service ID, its HTTP or HTTPS endpoint and an access token, then choose it in Card settings. The Card's `executionService` holds that service ID. [Run a Card on an execution daemon](RemoteExecution.md)
- A Card on a registered daemon must keep **Restrict file access** on, and cannot use `reuseWorker` or host folders. Its timeout can be at most seven days. When the daemon reports that it can enforce them, the Card can set CPU and memory limits (`executionResources`), which apply to the whole job, its build included.
- The daemon keeps submitted jobs in a durable queue and continues them after it restarts. Each invocation records the service ID, the job ID, the architecture, the limits and the last confirmed state of the job, shown under **Info** in the Card's run history.

### Another system (x86_64-linux)

- A Card can be built and run for another Nix system using **CPU architecture** in its settings (`"system": "x86_64-linux"` in `_card.json`), for software with no aarch64 build. The value is `x86_64-linux` or `aarch64-linux`. For the local service the list offers the native system and each system with a registered emulator, and for a registered daemon the systems that daemon reports. nixpkgs is imported for that system, and the kernel runs its binaries with an emulator registered with binfmt_misc.
  - **Mac (the shared VM):** Rosetta when it is installed on the Mac. qemu (`qemu-user` from cache.nixos.org) only when asked for with `TSUGUMI_VM_X86=qemu`, for a Mac without Rosetta or to try the qemu path. The app never installs Rosetta itself.
  - **Linux:** whatever the system registered, such as `boot.binfmt.emulatedSystems` on NixOS (with Nix's `extra-platforms`).
- Each invocation records the emulator that ran it (its path; qemu's includes its version), shown in the Info tab of the Card's run history. Rosetta and qemu need not give the same numbers, so the record tells them apart.
- The kernel runs Bash Cards' programs under the emulator. Package Cards load a `main.so`, so their build also makes a worker for the selected system (`worker` of the Nix SDK) from the Card's own nixpkgs, and the kernel runs it under the emulator. The worker and Card use the same target runtime libraries. It is built once per nixpkgs and system, under emulation the first time (on an M3 Max: the first C++ Card's build stage took 2 min 16 s with Rosetta, the x86_64 compiler's download included, and the worker alone 4 min with qemu), and reused from the store after that by every such Card, whichever emulator built it.
- Without an emulator, the run stops before building, saying what is missing.

### Outside access (Experimental)

- By default a Card has no network and cannot see the Mac's file system ([security.md](security.md)).
- In the Experimental section of **Card settings → General**, "Allow network access" (`network`) and "Allow access to folders of the host" (`hostFolders`; seen from the worker as `/host/<name>`, read-only by default) can be set per Card.
- "Restrict file access" (`sandbox`, on by default) is in the same tab. An Agent's save that turns it off, allows the network, adds a host folder or changes the execution service waits for a person's approval ([The Agent's permission boundary](security.md#the-agents-permission-boundary)).

## 5. Documents and saving

- A project is an ordinary folder of `tsugumi.json`, `definitions/<Card>/` (`_card.json`, `_meta.json` (intentions such as the summary), `default.nix`, `src/`, `_outputs.fbs`) and the layout in `layout.json`, (plus `_link.json` and `externals/` for a Card in Card), and can be managed with Git as it is. The hidden `.tsugumi/` folder holds the project id, the save journal and the run history.
- Every JSON file is validated strictly against a typed schema (UTF-8; duplicate keys, non-finite numbers and unknown keys refused) before use. The core does not re-encode the document text it receives (apart from `layout.json` when a Card is renamed or deleted, and `_meta.json` when an empty description is filled), so the notation of 64-bit integers and the like is kept as it is. JSON files under `src/` are sources and are not schema-validated.
- **Save transactions**: apply provisionally in memory and validate → `fsync` a journal (the contents of all files and a receipt) → replace the target files atomically → commit. Should it crash in the middle, the journal is applied again at the next start, so the result is always "all applied" or "none applied" (tested with fault injection).
- **Conflict detection**: a save is based on the revision at which editing began. If another window or the Agent saved first, it is detected as a conflict (`revision_conflict`, or `hash_conflict` per file) and nothing is silently overwritten. A file edited outside Tsugumi is reported as `external_change` and must be reloaded before saving. A resend with the same `op_id` is applied only once (if a response is lost to a dropped connection, `OperationGet` confirms the result).
- **Saving during a run**: while a Workflow runs, a save that changes the computation's definition (its execution fingerprint) is refused with `busy`; only layout changes and the like are accepted, safely. Reload, version checkout, cache clearing and history deletion are refused during a run.

## 6. Run history and artifacts

See the [run history reference](RunHistory.md) for the workbench controls, examples and retention settings.

- Runs, invocations, stages, values, logs, displays and receipts are recorded in SQLite. The shared Mac VM keeps its primary copy on a dedicated history disk (`vm/history.img`). That disk survives an environment reset.
- `recordIO` controls input and output value recording. FlatBuffers values retain their schema by hash, so past values can be interpreted after a Card's types change. `retainArtifact` copies a selected File or Directory into history and checks retained files for tampering and path escapes.
- `saveRunHistory` writes finished records into the project’s `.tsugumi/runs.sqlite`. When a copied or moved project opens, records missing from its local history are imported by run ID (runs with remote jobs are not exported). `saveRunHistory` is on by default.

## 7. Visualisation

The [visualisation templates](Visualization.md) provide editable Matplotlib, Plotly and Altair examples. [marimo notebooks](Marimo.md) and [R reports](Reports.md) retain HTML results that can be inspected in the [static artifact preview](RunHistory.md#keep-and-open-artifacts).

- **A Card can bring its own view.** Declare a JavaScript view output in Nix and
  send a small JSON config plus named binary blobs. WebGPU, DOM and bundled
  libraries run directly inside an isolated frame, with interaction independent
  of the worker. Both UIs fetch immutable code from the core through RPC and
  keep the existing canvas previews. [Writing a view](CardGuide.md#card-owned-web-views)
- Calling display APIs from a Card's code (`tsugumi::visualization::text(...)` in C++, `ctx.display_text(...)` and the like in Python) displays text, Markdown with TeX math, scalars, line charts, tables, images (PNG or JPEG), 3D point clouds and Card-owned web modules (including Plotly and Vega-Lite).
- Line charts are drawn fast with WebGPU: Dawn (Metal backend) in the Mac app, the browser's own WebGPU in the Web UI. While a Card runs, its frame keeps the last settled display (dimmed) unless the Card's "Chart updates" setting is "Live", in which case it updates in real time.
- matplotlib in a Python Card draws straight into the Card's display through the SDK's backend (`tsugumi.mpl`): code written for a window runs as it is, like X forwarding. `plt.show()` shows a figure (figures still open are shown at the end of a successful invocation, unless the Card sent a display of its own), and loops with `plt.pause()` and `FuncAnimation` update live during the run. Cards that use matplotlib get a Japanese font (IPAex) unless the Card sets its own `fonts`, so Japanese labels draw as they are.
- Plotly's `fig.show()` and Altair's `chart.show()` appear in the Card's display as interactive charts (zoom, hover and legend operations in the enlarged view). They are drawn with the SDK-pinned plotly.js / Vega view outputs and never go to the network (a Card's charts cannot read data from outside). See the Plots demo ("Plots (matplotlib, Plotly, Altair)").
- A display is a separate frame on the Canvas that can be placed and enlarged freely, and its layout is saved in `layout.json`. Renaming a Card moves its position, frame and group to the new name, and removing it from the Canvas removes them with it (the same whether from the UI or the Agent, through `tsugumid`'s save).

## 8. UI (the workbench)

- The sidebar on the left: "Workflow Hierarchy", "Add Card" (definitions of imported projects, and definitions not placed) and "Cards Shown". At the bottom, buttons for "Version History" and "Project Settings".
- The Canvas in the centre: one floating row at the top with breadcrumbs through the hierarchy, the run operations (with the issue list and a "+" Add Card menu), "Generate Cards" (only when the scope has Drafts), "Generate from comments (N)" (only when Cards have open comments), and buttons for going back Home and for the chat. Below, zoom, Fit All, Arrange, Untangle Wires and grid controls, the Nix references toggle and a status bar, and under them the collapsible log, timeline and terminal panels.
- On the right: the chat is the right-hand column of the workbench; its width can be changed and it can be a separate window.
- Canvas operations: adding and moving Cards (snapping to the grid while it is shown), folding Cards, duplicating, wiring, range selection, zoom, arranging (a layered layout with straight wires, in which Cards, output frames and sticky notes do not overlap, undone in one step), groups, Markdown sticky notes and display frames.
- "Untangle Wires" reduces crossings by reordering the visible ports while keeping Card positions. Arrange also runs this step after placing Cards. Both operations undo in one step. Port names, types, connections and execution stay unchanged; only the visual order is saved in `layout.json`. The pass considers routed wires, output-frame obstacles and child Workflow boundary wires, and keeps a proposed order only when it reduces crossings. Routing Cards retain their shape's port order; some graphs still have unavoidable crossings.
- The Card settings window: General, Comments, Nix Definition, Source (syntax highlighting, a "Format" button, showing where errors are), Ports, Run History, Artifacts and Extra Context (the full list is for Code Cards; Input, Draft and Card in Card Cards show only the tabs that apply). Edits are made on a working copy and put in with "Apply".
- The settings window: the whole app (General, Appearance, Editor, Shortcuts, Skills, API, External Connections, Binary Caches and Execution service; the Web UI has no Binary Caches pane) and the open project (Name and Environment, Canvas, Run history, Build Cache). Each pane shows where its values are kept.
- Custom signed [binary caches](BinaryCaches.md) (Attic, Cachix and compatible servers), configured for the Mac shared execution environment, including private-cache authentication. Changes apply at the next VM start.
- The [project terminal](Usage.md#project-terminal) opens in the project's execution environment. On Mac, it waits for the environment to connect and shares its tabs between windows of the same project. A tab whose shell ended gets a new shell when the environment connection comes back.
- Japanese or English, chosen under "General" in the settings (Japanese by default, whatever the system's language; it takes effect when the app restarts, and the Web UI reloads the page by itself). The Mac app and the Web UI on the same Mac share it, and what tsugumid writes into a project's git for a run follows it ([Versions](Versions.md#the-results-of-runs)).
- While running, changes to Cards and closing the project are blocked; progress, history and artifacts can still be seen.

### Several projects

- The app shows one project (or Home) per window. ⌘⇧N adds a Home window, and ⌘N, ⌘O, demos and duplicates from a project's window open in a new window. A project already open has its window brought to the front.
- ⌘W saves and closes only that project (not while it runs), and ⌘Q asks about the unsaved changes of all projects at once.
- What is separate per project is `tsugumid` (document, runs, history), the chat and Agent, and tabs and selections. The shared VM, `/nix/store`, CPU and memory, API keys and app settings, and Home's guide conversation are shared. Closing one leaves the other running.
- The Web UI has one per browser tab ([Several projects in the Web UI](WebUI.md#several-projects)).

## 9. Agent

The Agent runs as the `tsugumi-agentd` service (`core/src/agent/`; the contract shared with the frontends is in `agent/Shared/`). The Agent is one client using the same RPC as the UI; without the Agent, making, editing and running Cards can all be done on screen.

### Commands in the project environment

The Agent's `project_shell` tool uses the public `ProjectShell` RPC, enabled only by the isolated project launcher. It runs as the project's user in `/project`, never as a fallback shell on the Mac or Web host. Agent use needs separate human-granted **Shell** access in addition to Read, Write and Run. Shell access includes the project's shared folders and the VM network; it is broader than an individual Card's permissions.

Commands and tool results remain in the conversation, with exit status, signal, timeout and cancellation. Each stream is bounded to 256 KiB and reports truncation; the default deadline is 10 seconds and the maximum is 30 seconds. Disconnecting cancels the process group. An operation ID is durable and cannot run twice after a retry or restart; an interrupted operation with an unknown outcome must be inspected before issuing a new one. Commands that edit project documents need `ProjectReload` before subsequent inspection or execution. These commands are recorded as tool operations, not fabricated Card invocations.

The ordinary Web host daemon does not enable this tool. Web clients connected to the Mac VM can use the same RPC and approval boundary. The Web UI's human terminal is distinct from the Agent tool: with the Mac VM it is the project's shell in the environment, as in the app, and on an ordinary Web host it is a shell of that host in the project's folder.

### Choosing Claude's features by purpose

Tsugumi uses Claude's models and features by choosing the one that fits each job. Each row's "Where to check" is code and tests.

| Purpose | Feature used | Where to check |
| --- | --- | --- |
| Make, fix, run and check Cards | Tool use in the Messages API. The tools are generated from the RPC contract (`protocol/tsugumi.fbs`), so the Agent has exactly the operations of the screen | [Tools and the RPC](#tools-and-the-rpc), `core/src/agent/catalog.cc`, `tests/core/agent/` |
| A model sized to the job | Implementing Cards uses the model the user chooses (default `claude-sonnet-5-5`, with effort and, on models that offer it, Fast mode selectable). With an Anthropic API key, the guide's conversation, the summaries of the thinking of the bird on the Canvas and the version summaries use the small `claude-haiku-5-5` (other providers use a small model of their own or the Agent's model) | [Connecting to models](#connecting-to-models), `modelOptions` in `agent/Shared/agent.json`, [Version summaries](Versions.md) |
| Judging from the figures a run makes (multimodal) | `run_record` passes the images a Card displayed (charts, RGB images and so on) to the model as images. The same in the app, the Web UI and the Claude Code plugin | `core/src/agent/tool_images.cc`, `web/server/mcp.ts`, `tests/core/agent/project_rpc_test.cc` |
| Implementing independent Drafts at the same time (subagents) | `delegate` takes 2 to 8 tasks, runs up to 4 helpers at a time, and returns their reports | [Splitting the work](#splitting-the-work-delegate), `tests/core/agent/project_rpc_test.cc` |
| Guiding through a fixed procedure (Skills) | The built-in Skill `$demo` (`agent/Shared/demo-skill.md`) guides through a demo, and Skills the user adds are loaded too | [Skills and conversations](#skills-and-conversations), [the guide](#the-thrush-chat-and-guide) |
| Showing work in progress | Tool arguments are received by streaming (`eager_input_streaming`), and the Card's code is shown on the Canvas as it is written | [The Agent on the Canvas](#the-agent-on-the-canvas), `core/src/agent/provider.cc` |
| Making long work faster and cheaper | Prompt caching on the system prompt and the tool definitions | `core/src/agent/provider.cc` |
| When up-to-date information is needed | The web search server tool (off by default; only when the user turns it on) | [Web search](#web-search) |
| Working with outside agents (MCP) | The Claude Code plugin hands Tsugumi's tools over as an MCP server, and the work appears in the Web UI. A save that widens a Card's permissions waits for a person's approval | [Using it from Claude Code](claude-code.md), `test_claude_code_plugin.py` |
| Guiding without an API key | A scripted guide that uses no model (`agent/Shared/guide-script.json`). Sends nothing anywhere and costs nothing | [The thrush's guide](#the-thrush-chat-and-guide) |

Besides Anthropic's Messages API, OpenAI's API and the ChatGPT plan are supported too ([Connecting to models](#connecting-to-models)).

### Connecting to models

- Supports the Anthropic Messages API and the OpenAI Responses API, responding by streaming. OpenAI can be used with an API key or with the user's ChatGPT plan (Sign in with ChatGPT, the procedure for open-source tools) ([Security](security.md#the-chatgpt-plan-sign-in-with-chatgpt)).
- The chat's model menu chooses effort and Fast mode. The model list comes from the provider; the effort levels and Fast mode offered for a model are only those confirmed in the provider's documentation (`modelOptions` in `agent/Shared/agent.json`). If nothing is chosen, these fields are not sent and the model's defaults apply.

### Tools and the RPC

- The definitions of the tools the Agent uses are generated automatically from `tsugumi.bfbs` (the FlatBuffers schema with its doc comments), and the conversion between tool arguments and FlatBuffers messages uses the same schema. Updating the RPC contract updates the Agent's tools with it.
- The Agent gives each tool call that changes something a unique `op_id`, and it goes through the same revision checks, conflict detection and confirmation notifications as the UI (an Agent save also gets extra checks: comment stamping, the Draft-intent refusal and approval of widened permissions).
- Attach a Mac folder to the chat and the Agent can read it with `attached_folder_read` (Mac only; shared read-only with that project only and needing its own Read grant; what is read is sent to the model).

### Web search

- Turn it on under "API" in the settings and the chat's Agent can search the web at its own discretion. Off by default; while off, the model is not shown the tool. That search terms go outside, and the risk of unpublished data being in them, are in [Security](security.md#web-search).
- Search uses the provider's feature (Anthropic's `web_search`, OpenAI's `web_search`; the same with the ChatGPT plan). Tsugumi does not connect to a search service. The local fixture has no search.
- A search appears in the chat as a row like other tools, so its terms and the URLs found can be followed. Sources the response cites appear as links under it. With the ChatGPT plan, only the cited sources appear (because `include` is not sent).
- Only the chat's Agent can use it; helpers and the guide's thrush use the same setting.

### Links to the evidence

Links in chat can open the current Card and an exact saved result. Mac and Web resolve these addresses within the open project. Missing Cards, runs, invocations, logs and artifacts appear as unavailable text. An invocation link opens its recorded display and input/output values while the Canvas keeps showing the current result.

| Markdown destination | Opens |
| --- | --- |
| `card:<id>` or `card:<outer>/<inner>` | The current Card on the Canvas |
| `run:<UUID>` | The run and its Card records |
| `run:<UUID>/<Card path>#<invocation>` | The cited invocation's saved result |
| `log:<UUID>/<Card path>?invocation=<number>&stream=stderr&offset=<bytes>` | The log at that byte offset (`stdout`, `stderr`, `build`, or `journal`) |
| `artifact:<UUID>?id=<artifact id>&path=<member>` | The saved artifact; omit `path` for the whole artifact |

Use these as Markdown links, such as `[spectrum](card:spectrum)`. URL-encode the query values. A Card link points to its present definition. An invocation link identifies the particular result you want to share.

### Generating Cards from Drafts

- The Draft Cards in scope are given to the Agent by key. After the access grant it reads their summary, processing, additional context, ports and the wires around them, saves the types, the Nix definition and the source code (one Card per save, each save one transaction) and then checks them by building and running.
- The order is "write first, run later". The Agent finishes writing as many Cards as it can, then runs once from the trigger to check. It runs in between only when writing the next Card needs information that only a run can give (the columns of a downloaded file, say).
- On failure or cancellation in the middle, the save in progress is not applied (the plan stays); Cards already saved remain.
- The intention written in a Draft stays as it was. A summary and additional context that were empty are filled in by the Agent from the instructions in the chat and the Draft's content (it adds nothing that was not said). `tsugumid` refuses a save that leaves them empty or changes what was written.

### Generating Cards from comments

Write "I want this changed" on a Card and have the Agent follow it. If a Draft is the plan of a Card before implementation, a comment is an instruction to the Card itself. Any kind of Card can have them.

- Write one with "Comments…" in the Card's context menu, or from the badge at the top right of a Card on the Canvas ("Comments" in the Card's settings). Adding, editing, resolving, reopening and deleting are saved on the spot (ones written during a run are saved when the run ends, like other edits).
- "Generate from comments" on the Canvas hands the unresolved comments to the chat's Agent: only the selected Cards' comments if the selection has Cards with open comments, otherwise all of those at the level shown (the button shows the count). The chat shows a fixed note that Cards are being generated from comments; the Agent reads the comments itself after the grant.
- By default the Agent rewrites the Cards that have comments, and adds or splits Cards if a comment asks for it. It saves with the same tools as for Drafts, and a save that widens a Card's permissions needs the same approval ([The Agent's permission boundary](security.md#the-agents-permission-boundary)).
- The Agent can write, edit, resolve and delete comments as people do. In return, who did what is kept.

**Who did what**

- For the Agent's saves, `tsugumid` writes the record from the save's `origin` (`core/src/comments.cc`). Whatever the model writes in these fields is not used.
  - `author`: who wrote it. `editedBy`: who last changed its text. `stateBy`: who last resolved or reopened it.
  - `cards`: the Cards changed by the save that resolved the comment. Empty if it was resolved without changing anything.
- In a Card's settings, comments the Agent wrote or changed carry an "Agent" mark, and the row of a resolution shows what was done and the Cards that changed. Resolved ones are folded, not removed, and anyone can reopen them.
- Under the chat's "Save changes" row is how that save changed the comments (`comments` in the save's result).
- The details of a version in the version history show how the comments changed in that version (added, edited, resolved, reopened, deleted), who did it and which Cards changed ([Versions](Versions.md#comments)).

**Where they are kept**

In `comments` of `definitions/<id>/_meta.json`. They do not affect execution, so results are not invalidated. They follow a Card when it is renamed, and a duplicate gets them too.

How they differ from sticky notes: a sticky note is a memo for people, placed anywhere on the Canvas, kept in `layout.json`, with no route to the Agent. A comment belongs to a Card, has a state and a record, and is part of the version summaries.

### Splitting the work (delegate)

- Independent work (Drafts not connected by wires, for instance) can be split by the Agent among helper Agents and done at the same time.
- A split is 2 to 8 tasks (`delegate` in `agent/Shared/agent.json`), each with a name, the root-level Cards it owns and an instruction. The core runs up to four helpers at a time and gives each at most 15 steps.
- Each helper has its own conversation and the same tools except `delegate`. The tool's contract is that a helper changes only the definitions of the Cards it was given (not `tsugumi.json`, the layout or other Cards).
- A helper starts from the parent's instruction alone, so the instruction must say what the Drafts and rules do not.
- The `runs` argument of `delegate` (`each` by default, or `together`) is in the tool's schema and the rules, which tell the Agent to choose `together` unless a helper needs the results of its own run; the core itself does not act on it.
- When all helpers have reported, the Agent checks each one's Cards and reports. The helpers' conversations are kept with the original conversation in the Agent core's database, so a half-finished split can be resumed.

### The Agent on the Canvas

While the Agent works, a thrush flies over the Canvas showing what it is doing (it does not take clicks).

- Reading a Card, it perches on that Card; writing a Draft up into a code Card, it draws as if drawing a line; for a new connection it threads a string; running, it flies to the start Card. Its speech bubble says what it is doing now.
- While the Agent writes a file into a save's arguments, the file name and the last few received lines appear next to that Card without simulated typing. `PutSource` shows file content; `PatchSource` shows only the current replacement text, explicitly marked as a patch. The latest file or patch replaces the previous one immediately, and a late path reveals the content already received. On the Mac the bird plays its scenes one at a time, and the scenes of a change that was already saved (writing its Cards out, threading its wires) are not cut for the next file: that file's panel appears once they have played, with everything received by then. Panels say "Not saved" (a patch: "Patch · not saved") until the save succeeds, disappear on failure/cancellation or when a run starts, and follow the Agent visibility setting. They take no clicks and hide below 50% canvas zoom. Helpers each have one panel in their own color.
- When the work is split, a thrush of a different colour is added for each helper, and the parent Agent's thrush is gold, named "Agent".
- On the Mac, turn on "Draw thoughts" (the button under the input field) and a small model summarises the Agent's messages and tool results into one sentence for the speech bubble (it uses tokens apart from the Agent, so it asks the first time it is turned on).
- On the Mac, its look is chosen under "The Agent at work" in "Appearance" of the settings: a thrush, a dot or nothing. The Web has neither this setting nor "Draw thoughts".

### Skills and conversations

- Skills (`skills/` in a project, or folders added in the settings) are called with `$`.
- Conversations can be listed, searched in full text and compacted by summarising.

### The thrush (chat and guide)

A project's chat is the thrush.

- The bird waits at the top left of the chat (above the chat button when the chat is closed). To explain something on screen it flies there with `guide_show` (▶ Run, a Card, a demo's card and so on) and says a word in a speech bubble.
- The speech bubble offers replies to tap ("Next", "Tell me more" and so on), buttons that act (open a demo, run, stop, open a Card's settings, the API settings, a new project and so on) and a field for a free reply. A tapped reply reaches the chat as the user's message.
- Closing the speech bubble sends it back. With reduced motion, it appears in place. ⌥⌘G opens and closes the chat (on Home, the conversation panel).
- The thrush guides step by step during a demo's guide. It starts when a demo's guide starts the built-in Skill `$demo`, or when you write `$demo` in the chat, and goes from explaining the Cards → running → explaining the results, ending with a farewell (the flow is in `agent/Shared/demo-skill.md`).
  - Outside the guide it is an ordinary chat. Asked how to use a feature, the bird flies to the button and says a word only when showing it on screen helps. It opens closed panels (logs, timeline, terminal), the version history or a Card's settings only when really needed.
- What the bird can point at are the main buttons of Home and of a project, Cards, the items of a Card's settings and "Apply" (the list is `guide_show` in `agent/Shared/agent.json`). It also flies inside a Card in Card (`card:<outer>/<inner>`, with the Canvas opening it) and into a Card settings window the thrush opens (`open`, `param:<name>`). While a project loads, it flies in over the bird of the loading card, matching its size, and comes out from there when loading ends.

On Home, a single guide conversation for the whole app (`Agent.Role.guide`; its rules are `agent/Shared/guide-prompt.md`) speaks the same way from the bottom right.

- The guide has only the reading tools (including `demo_list` and `demo_read` for the bundled demos) and `guide_show`. It does not save, run or stop.
- The guide answers with a small model: `claude-haiku-5-5` for Anthropic, the luna model for the ChatGPT plan, and the Agent's model with an OpenAI API key. A demo's guide is carried on by the copy's chat, with the chat's model.
- Conversations, the guide's included, are kept in the Agent core's database (`agent-core/agent.sqlite` in the support folder; `~/Library/Application Support/Tsugumi/agent-core/` on a Mac).
- On first launch, choose Anthropic or OpenAI, enter one API key and confirm the connection before starting ("Skip and start" is also possible). The key is saved in the Keychain by the same route as "API" in the settings. If a usable key (or the ChatGPT plan) already exists, nothing is asked.
- The first time it appears it asks "What would you like to do?" and offers "Look at an existing Workflow" and "Have AI implement Drafts". The former asks for a field (`field` in `demos/en/index.json`) and then a demo in it; the latter asks for a Draft demo. Up to here the app asks with fixed text, so no model is used. It flies to the chosen demo with the demo list open and offers "Open this demo" ("Other demos" chooses again). Only when you write a question or your interests, or press "Ask for a recommendation", does the guide answer with a model, choosing demos that fit by field, description and tags. Once a demo is opened, the thrush of that copy's chat carries on the guide.
- While not connected to a model (no API key and no ChatGPT sign-in), the bird still appears and answers from a fixed script (`agent/Shared/guide-script.json`, the same file read by the Mac and the Web). It sends nothing anywhere and costs nothing.
  - The script can guide through copies of three demos: Data to report, Kepler-6 b and Pharmacokinetics. It starts on opening one, or on sending `$demo` in its chat, and proceeds by following the choices. The results of a run lead on separately for success and failure.
  - In other demos and projects, it offers buttons to open these three. To a free question it replies that connecting Claude (an Anthropic API key) or Codex (an OpenAI API key or ChatGPT sign-in) under "API" in the settings lets it answer, and that `$demo` starts the guide in a demo's copy.
- The first time the user connects to a model by saving a key (or signing in to ChatGPT), the main window in front asks once "Would you like to see the guide?", noting that the guide uses the model's API and costs money. "See the guide" opens the Data to report demo and starts the guide; "Later" is the same as "Later" in the speech bubble.
- During the guide, the start and end of runs (succeeded, failed, stopped) and disconnections and reconnections are reported to the chat as they really are. What happens while it is answering is reported after it finishes. The guide's state and the answer to the invitation are saved and not repeated after a restart.

## 10. Demos

The bundled demos, including Draft versions, come in a Japanese set and an English set (`demos/ja`, `demos/en`): the same demos and files, their descriptions, notes and the text of their figures in one language. The app and the Web UI list the set of their language. What you can try, and how each demo's Cards connect and behave, is in [The bundled demos](Demos.md).
Choose "Open a Copy" on a demo from "Browse Demos" on the home screen to get an editable copy in `~/Documents/Tsugumi/<demo>` (`<demo>` is the demo's id; `-2`, `-3`, … is appended if the folder exists).

For the design and implementation, see [Rationale](Rationale.md), [Development](Development.md), [Security and isolation](security.md) and [Limitations](Limitations.md).
