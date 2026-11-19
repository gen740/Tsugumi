# Tsugumi's features

Tsugumi connects small, typed steps called Cards into a Workflow. Run the whole graph, change one step, and inspect the recorded conditions and results. Use the macOS app or open the workbench in a browser with the `tsugumi` command. [Installation](INSTALL.md) · [First steps in the workbench](Usage.md#the-first-five-minutes)

## Find a feature

### Build and navigate a Workflow

- **Sketch before writing code.** Add a Draft with `e` while the Canvas has focus, describe a step, and draw its wires. Add a Start, Input, Switch, Fork, Branch, Merge or Collect Card when you already know how data should flow. [Canvas tools](Usage.md#canvas-tools) · [Kinds of Card](#kinds-of-card)
- **Organise a larger graph.** Right-click the Canvas and choose **Arrange** to place Cards or **Untangle Wires** to reduce crossings while keeping their positions. Group related Cards with **Group Selection**, or open a Card in Card to see its child Workflow and boundary ports. The import button by **Add Card** copies another project into `externals/` and places its Workflow as a Card in Card. [Canvas tools](Usage.md#canvas-tools) · [Cards and Workflows](#2-cards-and-workflows)
- **Sweep inputs and implementations.** Give a Card a list of implementations in `run`, or send it a batch, and it runs every combination. Connect the batch to a Collect Card to pass one vector or Directory on. [Run lists and Collect](#run-lists-and-collect)
- **See references between Card definitions.** Turn on **Nix references**, then hover a Card or open the list. Follow `inputs.data` to the Card connected to the `data` input. [Nix references](Usage.md#canvas-tools)

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
- **Pin dependencies and control state.** Use **Project settings → Name and Environment** to pin Nixpkgs or add a trusted flake. Declare a Card-local lock where the SDK supports one (Python, Ruby, Go, Rust and C#). For a Card that needs interpreter or library state, turn on **Keep the worker's state** (`reuseWorker`) in its settings. [Nix environments](#3-reproducible-environments-with-nix) · [Language SDKs](CardGuide.md) · [Worker state](CardGuide.md#worker-state)
- **Keep a monitoring worker resident.** Workers retained with `reuseWorker` have no count, idle-time or memory eviction limit. They stay until the environment stops or the Card's definition or execution conditions change; a failed, cancelled or timed-out call discards its worker, and a dead worker is replaced. Each invocation records reuse, and its log identifies replacement of resident state. Fresh workers prepared for speed keep their separate limits. [Worker state](CardGuide.md#worker-state)
- **Use a shell or a signed build cache.** Open the project Terminal to run a tool such as `nix --version`. On Mac, configure a shared signed cache in **Settings → Binary Caches** and apply it on the next VM start. [Project terminal](Usage.md#project-terminal) · [Binary caches](BinaryCaches.md) · [Browser and CLI platforms](WebUI.md#the-tsugumi-command)
- **Run a Card on another Linux machine.** Start `tsugumi-workerd` there, register it in **Settings → Execution service**, and choose it under **Execution service** in the Card's settings. [Run a Card on a remote worker](RemoteExecution.md) · [Execution service](#execution-service)
- **Control a Card's access.** Use **Card settings → General** to review **Restrict file access**, and the network and host folder permissions under **Experimental**, before running code from another project. [Security and isolation](security.md#summary)

### Work with the Agent and explore examples

- **Turn a Draft into a Card.** Connect a model under **Settings → API**, describe an operation in a Draft and choose **Generate Cards**. Review the generated source, ports and run record. Use Card comments to request a specific change. [Generate from Drafts](#generating-cards-from-drafts) · [Generate from comments](#generating-cards-from-comments) · [Data and permissions](security.md#7-the-models-api-keys-agent)
- **Start from an example.** Browse Demos and open a copy to change a parameter, run it again, or follow the thrush's guide. Use the [documentation site](https://gen740.github.io/Tsugumi/)'s recordings to inspect example Workflows before installing. [Bundled demos](Demos.md)
- **Work from Claude Code.** Install the plugin to have Claude Code make and run Cards through Tsugumi's tools, with the work shown in the Web UI. [Claude Code plugin](claude-code.md)

## 1. Architecture

The source tree follows program ownership: `tsugumid/document` contains definition
contracts; `tsugumid/runtime` owns project execution/history; `hostd` owns human
and Agent policy; `workerd` owns worker lifetimes and invocation values.
`util` and `rpc` provide mechanisms without importing those program policies.
The guest is built under `vm/guest`, the macOS VM helper under
`vm/macos/runtime`, and protocol-aware Swift clients under `client/swift`.
UI implementations are `ui/web` and `ui/macos`, with isolated module resources
and Card templates in `ui/shared`.

See the [trust boundaries](security.md#1-the-trust-boundaries), [RPC contract](Protocol.md) and [repository map](../AGENTS.md#map).

The host-side `tsugumi-hostd` exposes environment operations through `EnvironmentRequest` in `protocol/host.fbs`: state, start, stop and attached-folder changes. Its native adapter launches `tsugumid` on Linux; its signed Mac adapter boots the shared VM and applies the mounts hostd specifies. In both cases, the service owns readiness checks, endpoint and project registration, execution-broker authorization, concurrent starts and process-exit reporting. Host events carry state, progress and stopped notifications. On the Mac, quitting the app requests orderly host and shared VM shutdown, including work from other clients, after one confirmation if work is in progress. Closing the last window quits only when enabled in General Settings (off by default). Hostd and project cores do not exit for idleness; without the app, the resident host remains until explicitly stopped. An open Web UI can start it again through its existing reconnect. SIGTERM, SIGINT and SIGHUP also request bounded orderly shutdown; hostd exit or a lost Watch connection makes vmhost do the same. Archiving incompatible history requires an explicit start flag. `HostRuntimeRequest` reports runtime/store state without starting a VM.

The shared VM's CPUs, memory and store disk size are host settings (`vm.cpus`, `vm.memoryGiB`, `vm.storeGiB`), edited under **External Connections** in the Mac app's Settings and under **General** in the Web UI. The value 0 uses this Mac's defaults: all but two cores (2–8), half the memory (4–16 GiB) and a 64 GiB store. Changes apply at the VM's next start. A larger store grows the disk in place; a smaller one keeps the current disk until the environments are reset. After two minutes with no project running in the VM and no store operation, the VM's memory balloon takes the guest down to 1.5 GiB: the guest drops its page cache and macOS gets the memory back. Starting a project or a store operation restores the full size first; the guest has most of its memory available again within half a second.

The Mac app, Web host and `tsugumi` command use the same Host environment requests and events. `tsugumi-vmhost` exposes the private length-framed FlatBuffers contract in `protocol/vm_host.fbs` to hostd. It retains Virtualization.framework, virtiofs/vsock and terminal descriptor bridges because they need the app’s signature and entitlement. Clients display runtime snapshots and exchange terminal bytes through Host requests; they do not connect to the helper. Store checking/repair, cleanup, history deletion and reset are explicit Host runtime actions. The guest uses `fsync-store-paths = true` and `fsync-metadata = true`. A durable running marker detects an unclean stop: before any project starts, the next boot runs `nix-store --verify --check-contents --repair` as root. The first boot of the new protection on any existing store disk also checks once. Failures block project startup; the console and `/persist/store-health/check.log` retain repaired and unavailable paths. Shutdown stops projects, syncs, and remounts the store overlay and persistent disks read-only before poweroff. A failed remount retains the marker.

## 2. Cards and Workflows

Every Card has a permanent identifier and a separate human name in `_meta.json.name`; when omitted, the identifier is displayed. Names need not be unique. The core creates new identifiers from the creation name's slug plus a four-character random salt, checking uniqueness within the owning project. Renaming in either app or through the Agent changes only the name: folders, wires, saved inputs and past run records keep the same identifiers, including inside a Card in Card. The Agent sees both name and identifier and uses identifiers for exact references.

### Kinds of Card

| Kind | Role |
| --- | --- |
| Code Card | Defines its environment in `default.nix` and runs C++, Python, Bash, R, Julia, Ruby, Go, Rust or C# code |
| trigger | Where a Workflow's run starts (output port `out`) |
| input | Builds each message in `_outputs.fbs` from parameters with matching names, edited directly on the Canvas (for a File type, the chosen file is copied into the run) |
| switch | Sends an editable Boolean on `value`, for example to the `condition` of a Branch |
| fork / branch / merge | Distribution (`in` to the ports in `branches`), conditional branching (`value` and a `condition` message with `value:bool`) and merging (declared inputs to `out`, a union named by input port with root `tsugumi.merge.Merged`). A scalar Branch sends its value to `a` or `b`. A Branch that receives a batch sends the filtered results to both outputs, including empty batches. For bounded feedback, send a value back through a Merge and raise the Cards' `maxRuns` (default 1) as a safety cap and end the loop through a Branch; exceeding `maxRuns` fails the Run with `execution_limit`. When both `value` and `condition` are batches, a Branch runs every combination |
| Collect | Turns a batch into one value. Typed values become an `items` vector, and Files or Directories become a Directory |
| subgraph (Card in Card) | Handles a child Workflow as one Card. `boundary` maps the outer ports to Cards inside |
| Draft | An "unimplemented Card": write its name and content directly on the card, and connect it just by dropping a wire on it (it has no ports; the other end names the real port). Draft wire ends use an empty port name. Implementing one requires explicitly rebinding its wires to real ports in the same save. The Agent can later generate its implementation |

A Card in Card uses full Card paths to distinguish state and history; visual groups in `layout.json` do not affect execution.

### Typed ports

- An output port carries one wire. To send one value to several Cards it goes through a fork Card (so there is one way to write it, and the distribution is visible on the Canvas as a Card). An output with two or more wires is reported as `fan_out` and cannot run. Drawing another wire from an output already in use on the Canvas inserts a fork automatically (if the output is a fork or connects to one, that fork gets another branch).
- An output port's type is declared in a FlatBuffers schema (`_outputs.fbs`), and the type of each downstream input port is derived automatically from upstream. Structural compatibility is checked when wiring, and a merge builds a union type automatically.
- Values flow between Cards as FlatBuffers bytes. Automated tests check that 64-bit integers, enums, structs, unions and File / Directory pass between C++ / Python / bash (and R and Julia through an in-memory JSON adapter) without loss.

### Execution semantics (scheduler)

- A Code Card can wait for an arrival on all required inputs (`allInputs`) or trigger on each arrival (`eachInput`). An arrival is a single value or a batch of values. The setting is `trigger`, with the values `allInputs` or `eachInput`. A Merge consumes one arrival at a time.
- Bounded queues with backpressure, cancellation and an optional per-invocation deadline (`timeoutSeconds`; none by default for local runs). Each input port holds up to 16 unconsumed groups, and a run that can no longer progress fails with `execution_limit`. A Run can request concurrency from 1 to 64, with up to eight Code workers active at once. The iteration limit (`maxRuns`) counts how often the Card receives a set of inputs that triggers it.
- Downstream of an unselected scalar Branch output is `skipped`. A Card downstream of a failed, blocked or cancelled Card is `blocked`, with a reason. A Draft, and the Cards that need only its outputs, do not run and get no state; the canvas marks them as not run because of a Draft. Independent paths keep running.
- A run lives only while the `tsugumid` process that started it lives. Local and remote invocations left unfinished end as `failed` with failure kind `interrupted`; startup never imports old remote results or reruns Cards. Within one core life, remote reconnection resumes the same job. Regular status and log exchanges detect disconnection; there is no heartbeat or silence expiry ([Remote execution](RemoteExecution.md#host-authorization-and-following-a-run)).
- Partial runs exist only for debugging: AI-written code rarely works on its first attempt. Running one Card or from a Card downstream with saved inputs is not a result of the Workflow. Such a run is marked **Partial run** (Japanese: **部分実行**). Values computed from it carry their provenance downstream, including into later runs that consume them as saved inputs; those runs also show **Uses a partial run's result** (**部分実行の結果を使用**). Running the whole Workflow replaces these values and clears the marks. Explicit **Record I/O** and **Retain artifact** settings still apply, and kept results carry the run's marks. There is no selection of an earlier run as the input source.
- Partial runs with "Run from Here Downstream" or "Run Only This Card (Last Inputs)" reuse the inputs saved last time (including the contents of Files). When "Run from Here Downstream" reaches a merge, a Card that waits for all its inputs fills only the ports that receive nothing this time with saved inputs (an `eachInput` Card runs with only the values that arrived).

### Run lists and Collect

The [Signal detection sweep demo](Demos.md#signal-detection-sweep-signal-sweep) shows three inputs, two implementations and one collected result.

- A Code Card can supply a list of implementations in its Nix `run` attribute, such as `run = [ "echo first" "echo second" ];`. For each trigger, the runtime runs every combination of those implementations and the items arriving on its input ports. A single value or implementation contributes one choice. For example, two implementations and batches of three and four input values give 24 invocations. Nested implementation lists are invalid.
- Results follow the order of the input port names, the item order within each input batch and the implementation order in `run`. Parallel workers can finish in a different order. The Canvas uses thicker wires for batches and shows how many items they carry.
- An empty implementation list or input batch produces an empty output batch and starts no workers. Collect turns an empty batch into a typed empty vector or an empty Directory. With File or Directory inputs, Collect keeps each item in a numbered subdirectory, preserving its original name and any empty directories. Each entry in the Directory's ordered `items` has a `name` and relative `path`. Other File and Directory metadata remains in the producer's individual invocation records.
- There are at most 256 combinations per trigger, 4,096 worker invocations per Run and 32 MiB of serialized port values for all the inputs of one activation and for all its outputs. If one invocation fails, the runtime cancels its siblings and withholds the batch. The history keeps each invocation's outcome. A partial rerun with last inputs uses the whole saved batch, including saved Files and Directories.
- A collected value can be at most 16 MiB. Collect can copy up to 1,000 files and 64 GiB, with a total limit of 10,000 batch members and filesystem entries. Directory collection rejects symbolic links and special files.

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

- `tsugumi:default` uses the Nixpkgs revision bundled with the installed version of Tsugumi. New projects are created already pinned to the app's revision. To keep using it after an app update, open **Project settings → Name and Environment** and choose **Pin to This App's Nixpkgs**. You can also enter a revision and hash in the form `https://github.com/NixOS/nixpkgs/archive/<rev>.tar.gz#sha256-…`.
- Nix evaluation uses restricted eval and forbids IFD (Import From Derivation), and the URLs it may fetch during evaluation are limited to nixpkgs archives (build-time fetches must be pinned by hash).
- When a run starts, every Code Card in it whose preferred worker is the local one is evaluated, in turn, and its build is handed to nix-daemon right away, without waiting for its inputs; nix-daemon decides how many build at once. A Card on a remote worker is built by that worker when it receives the invocation.
- Builds reuse outputs already in the Nix store. A signed [binary cache](BinaryCaches.md) can supply a missing store path before Nix builds it locally.

### Sharing Nix exports through connections

- Set `exports` in the upstream Code Card's `default.nix`. If a downstream Code Card receives its output on the `data` port, that Card can use the exported attributes through `inputs.data`. The same exports are available to each implementation in a run list.
- The name after `inputs.` is the downstream input port name. The port must resolve to one Code Card that always runs first. `tsugumi` and `sources` are reserved for the SDK and configured sources.
- Fork, Collect, the value input of a Branch and Workflow boundaries preserve the Code producer's exports. A Merge can pass them through when all its inputs come from the same Code producer. A wire with several possible producers still carries its data, but a Nix reference to that input is ambiguous.
- Static diagnostics describe directly written references and flag unknown or ambiguous ports. Nix also checks references reached through an alias during evaluation.

- The [Canvas overlay](Usage.md#canvas-tools) draws the static references from this resolution. Nested targets meet their visible container and the list preserves the full paths. An invalid reference remains visible with its diagnostic. Saving a source refreshes the resolution.

### External Nix sources (`inputs.sources`)

- Flakes can be added under "External sources" in "Name and Environment" of the project's settings. Enter an address (`github:owner/repo`, or the URL of a GitHub page) and press "Fetch" to see the commit and the list of packages for this environment, then press "Add". "Update" fetches a newer commit.
- Once added, it is kept in `tsugumi.json`, pinned by commit and hash. The supported `type` is `flake`.

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

### Keeping build results

Use [Build Cache settings](Usage.md#manage-build-and-scratch-space) to inspect and explicitly clear build registrations and store space. [Binary caches](BinaryCaches.md) explains shared caches.

## 4. Card SDK and workers

Start with [Your first Card](CardGuide.md), then use the [SDK reference](CardGuide.md), [worker state rules](CardGuide.md#worker-state) or [C# guide](CSharp.md). Inspect execution stages in [Run History](RunHistory.md#inspect-an-invocation).

### Execution service

[Register a remote worker](RemoteExecution.md) to build and run a Card on another Linux host. Review the [data boundary](security.md#remote-worker-boundary) before sending inputs.

### Another system (x86_64-linux)

- A Card can be built and run for another Nix system using **CPU architecture** in its settings (`"system": "x86_64-linux"` in `_card.json`), for software with no aarch64 build. The value is `x86_64-linux` or `aarch64-linux`. For the local service the list offers the native system and each system with a registered emulator, and for a registered remote worker the systems that worker reports. nixpkgs is imported for that system, and the kernel runs its binaries with an emulator registered with binfmt_misc.
  - **Mac (the shared VM):** Rosetta when it is installed on the Mac. qemu (`qemu-user` from cache.nixos.org) only when asked for with `TSUGUMI_VM_X86=qemu`, for a Mac without Rosetta or to try the qemu path. The app never installs Rosetta itself.
  - **Linux:** whatever the system registered, such as `boot.binfmt.emulatedSystems` on NixOS (with Nix's `extra-platforms`).
- Each invocation records the emulator that ran it (its path; qemu's includes its version), shown in the Info tab of the Card's run history. Rosetta and qemu need not give the same numbers, so the record tells them apart.
- The kernel runs Bash Cards' programs under the emulator. Package Cards load a native `main.so` or an explicitly named SDK runtime artifact, so their build also makes a worker for the selected system (`worker` of the Nix SDK) from the Card's own nixpkgs, and the kernel runs it under the emulator. The worker and Card use the same target runtime libraries. It is built once per nixpkgs and system, under emulation the first time, and reused from the store after that by every such Card, whichever emulator built it.
- Without an emulator, the run stops before building, saying what is missing.

### Outside access (Experimental)

- By default a Card has no network and cannot see the Mac's file system ([security.md](security.md)).
- In the Experimental section of **Card settings → General**, "Allow network access" (`network`) and "Allow access to folders of the host" (`hostFolders`; seen from the worker as `/host/<name>`, read-only by default) can be set per Card.
- "Restrict file access" (`sandbox`, on by default) is in the same tab. An Agent's save that turns it off, allows the network, adds a host folder or changes the execution service waits for a person's approval ([The Agent's permission boundary](security.md#the-agents-permission-boundary)).

## 5. Documents and saving

[The Card guide](CardGuide.md#the-files) describes project files. [ProjectSave](Protocol.md#projectsave) defines save transactions and conflicts; [Versions](Versions.md) explains returning to saved code and settings.

## 6. Run history and artifacts

[Run History](RunHistory.md) owns recording, inspecting, comparing, copying and deleting results and artifacts.

## 7. Visualisation

Use the [visualisation templates](Visualization.md), [marimo notebooks](Marimo.md) or [R reports](Reports.md). Authors of custom views use [Card-owned web views](CardGuide.md#card-owned-web-views); users control [chart updates](Usage.md#chart-updates).

## 8. UI (the workbench)

[Usage](Usage.md) covers Canvas tools, Card settings, project terminals and the Agent's controls.

### Several projects

See [Several projects](Usage.md#several-projects) for windows and [Web UI tabs](WebUI.md#several-projects).

## 9. Agent

Connect a model under **Settings → API**, then ask the chat to make or change Cards. [Permissions and sent data](security.md#7-the-models-api-keys-agent) · [Agent controls](Usage.md#working-with-the-agent)

### Commands in the project environment

Helpers have separate scratch and command/output directories, including unscoped helpers. A parent can hand a file to a helper by saving it in a Card included in that helper's `read_cards`; scratch is not shared. Cancelling before the durable save intent discards staged changes; once the journal intent is durable, recovery completes the atomic save.

Use `project_copy` to transfer an explicit list of `{from,to}` files from an attached folder, another Card or the caller's private scratch into Card sources. The host moves bytes through the normal save; the model receives only paths, sizes and SHA-256 hashes. Attached sources need their own Read grant, Card sources need read scope, and destinations need write scope (helpers' exact sets apply). Links and traversal are refused. Copy and shell saves stream file bytes through private durable staging files and the normal journal; file contents do not cross the RPC frame or require a whole-file buffer. There is no separate file-count or total-byte limit on a copy. Document JSON remains limited to 8 MiB and the source manifest to 10,000 entries. For larger data, keep the folder attached and let a Card read it with `hostFolders` at `/host/<name>`, after approval.

`attached_folder_read` and Agent `project_file_read` return bounded pages: default 256 KiB, maximum 1 MiB, with total size and continuation offsets. A 200 MB attached file reads only the requested page. The UI can still fetch every page. Model-facing pages are reduced further to preserve valid JSON and correct continuation offsets within the model tool-result budget. To inspect large content use `head`, `grep`, `wc` or bounded reads; move it with copy, never read and retype it.

Grant **Shell** separately from Read, Write and Run to use `project_shell`. Its cwd is a staged view with ordinary `definitions/<id>/...` and nested `externals/...` paths. It includes only readable Cards; writable Cards' sources can be edited with tools such as `sed`. Read-granted attached folders appear read-only at `attached/<SHA-256(folder path)[0:16]>/`. `scratch/` is writable and persists for that conversation or individual helper. Root `tsugumi.json` and `layout.json`, and each Card's `_card.json`, `_meta.json`, `_link.json`, `_inputs.fbs`, `_outputs.fbs`, `_params.fbs` are read-only; use `project_edit` for small changes to these (including the authored output schema). The real project and `.tsugumi/` are inaccessible. A successful, uncancelled command saves one validated Agent revision. Failure, timeout or cancellation saves no Card changes. The normal revision, hash and external-file conflict checks protect concurrent edits; a conflict is reported, never silently merged. Core executes inside the VM on Mac and directly on Linux, with hidden credentials/control files and blocked direct local RPC. Timeout defaults to 10 seconds, maximum 30 seconds. Each output stream returns at most 256 KiB, head and tail when truncated, with total sizes and a `full_output_path` prefix for complete `.stdout`/`.stderr` files. The model-facing result retains both ends and the total sizes within 60,000 bytes of JSON, including escaping or base64; full output stays in the files. Output, scratch and command views remain under the project's `.tsugumi/agent-files/` for the person to inspect with their file tools. Analysis still belongs in Cards, whose steps can be revisited. Both chat and Claude Code tools follow two lists in `hostd/agent/Shared/agent.json`: `requests` for ordinary operations and `approvalRequests` for operations that need the person's approval for each call. The Agent may inspect usage and propose deleting run history, project build caches or selected run scratch; the approval shows the targets and measured sizes (unknown measurements are explicit). Starting versioning, keeping a named version and checking out a version also need approval each time. Version-note generation follows the person's note setting: hostd reads and stores the note through internal project RPC, independently of the tool catalog; note writes are never model or plugin tools. Concurrent requests for the same full version object ID share one generation regardless of hex letter case. The core supplies the affected runs, cache, folders or version, and records the answer and outcome in the conversation. Declining changes nothing. View state and connection operations are on neither list. Reloading external edits requires approval for every Card permission widening, regardless of origin. [Shell contract and limits](Protocol.md#projectshell) · [Human terminal](Usage.md#project-terminal)

### Choosing Claude's features by purpose

Tsugumi uses Claude's models and features by choosing the one that fits each job. Each row's "Where to check" is code and tests.

| Purpose | Feature used | Where to check |
| --- | --- | --- |
| Make, fix, run and check Cards | Tool use in the Messages API. The tools are generated from the RPC contract (`protocol/tsugumi.fbs`) for the explicit `requests` allow list in `hostd/agent/Shared/agent.json` | [Tools and the RPC](#tools-and-the-rpc), `hostd/agent/src/catalog.cc`, `tests/core/agent/` |
| A model sized to the job | Implementing Cards uses the model the user chooses (default `claude-sonnet-5-5`, with effort and, on models that offer it, Fast mode selectable). With an Anthropic API key, the guide's conversation, the summaries of the thinking of the bird on the Canvas and opt-in Agent version summaries use the small `claude-haiku-5-5` (other providers use a small model of their own or the Agent's model) | [Connecting to models](#connecting-to-models), `modelOptions` in `hostd/agent/Shared/agent.json`, [Version notes](Versions.md#version-notes) |
| Judging from the figures a run makes (multimodal) | `run_record` passes the images a Card displayed (charts, RGB images and so on) to the model as images. The same in the app, the Web UI and the Claude Code plugin | `hostd/agent/src/tool_images.cc`, `ui/web/server/mcp.ts`, `tests/core/agent/project_rpc_test.cc` |
| Implementing independent Drafts at the same time (subagents) | `delegate` starts tasks with depth/count capabilities and exact Card sets, and returns their reports and errors | [Splitting the work](#splitting-the-work-delegate), `tests/core/agent/project_rpc_test.cc` |
| Guiding through a fixed procedure (Skills) | The built-in Skill `$demo` (`hostd/agent/Shared/demo-skill.md`) guides through a demo, and Skills the user adds are loaded too | [Skills and conversations](#skills-and-conversations), [the guide](#the-thrush-chat-and-guide) |
| Showing work in progress | Tool arguments are received by streaming (`eager_input_streaming`), and the Card's code is shown on the Canvas as it is written | [The Agent on the Canvas](#the-agent-on-the-canvas), `hostd/agent/src/provider.cc` |
| Making long work faster and cheaper | Prompt caching on the system prompt and the tool definitions | `hostd/agent/src/provider.cc` |
| When up-to-date information is needed | The web search server tool (off by default; only when the user turns it on) | [Web search](#web-search) |
| Working with outside agents (MCP) | The Claude Code plugin hands Tsugumi's tools over as an MCP server, and the work appears in the Web UI. A save that widens a Card's permissions waits for a person's approval | [Using it from Claude Code](claude-code.md), `test_claude_code_plugin.py` |
| Guiding without an API key | A scripted guide that uses no model (`hostd/agent/Shared/guide-script.json`). Sends nothing anywhere and costs nothing | [The thrush's guide](#the-thrush-chat-and-guide) |

Besides Anthropic's Messages API, OpenAI's API and the ChatGPT plan are supported too ([Connecting to models](#connecting-to-models)).

### Connecting to models

The host-side `tsugumi-hostd` owns provider endpoints, revisioned settings and OAuth credentials. The clients keep settings screens, presentation snapshots, browser opening and macOS login Keychain access. Keychain-backed API keys are provisioned to hostd in memory only; non-Mac and Web Keychain-save fallback keys use its owner-only store. Host RPC supplies settings get/update, secret-input provider set/status and sign-in begin/action/status; ordinary model selections contain no endpoint or credential. Desktop settings are shared; the app and Web hosts have separate ChatGPT account profiles. OAuth validation, callback handling, refresh rotation and revocation run in core. See [API keys and sign-in](security.md#7-the-models-api-keys-agent).

- Supports the Anthropic Messages API and the OpenAI Responses API, responding by streaming. OpenAI can be used with an API key or with the user's ChatGPT plan (Sign in with ChatGPT, the procedure for open-source tools) ([Security](security.md#the-chatgpt-plan-sign-in-with-chatgpt)).
- The chat's model menu chooses effort and Fast mode. hostd fetches and persists the model list from the provider; the effort levels and Fast mode offered for a model are only those confirmed in the provider's documentation (`modelOptions` in `hostd/agent/Shared/agent.json`). If nothing is chosen, these fields are not sent and the model's defaults apply.

### Tools and the RPC

See the [RPC contract](Protocol.md) and [Agent permission boundary](security.md#the-agents-permission-boundary).

### Web search

[Web search settings and data sent](security.md#web-search).

### Links to the evidence

[Follow evidence from chat](RunHistory.md#follow-evidence-from-chat).

### Generating Cards from Drafts

[Generate from Drafts](Usage.md#working-with-the-agent) after describing each step and connecting its wires.

### Generating Cards from comments

[Generate from comments](Usage.md#working-with-the-agent) to apply a request to existing Cards; [Versions](Versions.md#comments) records comment changes.

### Splitting the work (delegate)

Independent Drafts can be implemented by helper Agents. Try the [Dynamical systems gallery](Demos.md#dynamical-systems-gallery-dynamics-draft).

In either UI’s Agent settings, choose whether to allow unscoped helpers. Each spawned sub-agent receives its own depth/count capability; the main Agent has no spawn limit. Unscoped helpers are off by default so each helper’s work has a boundary a person can follow. The parent supplies separate `read_cards`, `write_cards` and `run_cards` arrays of exact permanent Card paths (`outer/step` includes only that Card). Inspection, files, saves, runs, records and logs enforce those boundaries in hostd; a container does not grant its children. A helper’s new `card_id` reservation grants it all three access kinds. Helpers may add their own reserved Cards and edit wires whose endpoints are in their read/write sets and which touch a writable Card. Small changes use `project_edit`, with keyed selectors and old/new values or exact text replacement, against the current document. The host preserves untouched document bytes. The canvas derives positions for Cards without saved coordinates. `project_connect`, `project_make_subgraph` and `project_layout` apply graph operations directly; `project_copy` moves files without reading their bytes into the model. Whole-document `project_save` is for creating a project and for a large restructuring; a stale revision is refused with a request to read again, keeping the proposed input. The parent handles project-wide settings, layout edits, container-link changes and tools that cannot identify a Card. A helper may implement a Draft and rebind its wires in one save when all incident endpoints are assigned; otherwise the parent finishes that transaction.

Helper usage adds to the conversation’s usage. A shortened delegate result says so and identifies its full trace in the conversation’s existing helper records.

### The Agent on the Canvas

[Agent activity and Draw thoughts](Usage.md#agent-activity).

### Skills and conversations

Conversation reads and event observation never start a turn. The core persists the operation in flight; interrupted conversations expose that operation and wait for an explicit **Resume** request or a new message. Tool receipt reconciliation remains the boundary against replaying an interrupted operation.

A Skill with front matter `scope: [read]`, `scope: [read, run]` or another inline list of `read`, `write`, `run` starts directly as a scoped helper. Choose it from a Card's **Run Skill** menu, or use `$name` in chat with selected Card references. One access prompt shows the Skill source and exact Card sets before the helper starts. No main Agent delegation is involved and this approval grants no project access. `always: true` cannot be combined with `scope`: automatic application has no selected Cards and cannot enforce a helper boundary. The Skill catalog reports this as an invalid declaration with its reason. A missing or invalid declaration cannot start a scoped helper; runtime instructions cannot add access. Run cancellation is limited to this helper's own runs, whose entire execution plan was checked against its run set.

The bundled `$review` reads permissions alongside their apparent uses, sources of nondeterminism, resident-worker state and the build/run boundary. `$rerun-compare` reads and reruns selected Cards under the same conditions, comparing output content using the existing run comparison rules; incomplete retained data remains unknown. They contain natural-language instructions, with Japanese and English text, rather than language-specific analysers. Findings cite `card:outer/step?file=src%2Fmain.py&lines=12-18`, which opens those source lines, and state whether they were only read or run and observed. Keep individual findings with **Keep finding as comment**; those comments carry `finding: true` and are excluded from generation requests. Nothing rechecks, resolves or deletes them automatically. Each helper's trace remains expandable in the conversation after reconnecting.

The host-side `tsugumi-hostd` discovers Skills, parses their metadata and reports unreadable or oversized files through `SkillsListRequest` in `protocol/host.fbs`. The app and Web host keep folder selection, completion and menus; they send selected Skill names. For names supplied by Tsugumi, one Skill wins: the trusted project’s `skills/` folder, then the person’s registered folders (the last matching source wins), then the built-in Skill. Other duplicate names retain source order. The service reads contents beneath registered host roots only after Read access is granted. A Skill must be smaller than 512 KiB. Discovery does not grant access to use its contents in a model prompt.

`ModelsListRequest` reads or refreshes the service's persisted catalog for each connection. Both clients use its `name` display field. An empty refresh leaves the stored catalog intact; a failed refresh does not change it. Clients retain only a presentation snapshot for rendering an open menu.

[Skills and conversations](Usage.md#skills-and-conversations).

### The thrush (chat and guide)

The host-side `tsugumi-hostd` embeds and interprets `hostd/agent/Shared/guide-script{,.en}.json`. `GuideGetRequest` reads a session in Japanese or English; `GuideAdvanceRequest` reports an interest, a message or choice, a demo opening, a run observation, or a completed client action. The reply contains the step, text, choices, semantic actions, highlight and panel request. Scripted guidance works without a model connection.

Scripted tours and the model-backed `$demo` tour give short guidance: run, open the result's location, optionally change a parameter, Apply and rerun. The person reads and judges the results. Tours do not require checklists or confirmations of saving and running, and do not infer output values or changes from a successful run. In a script, `finished` names the step after success; other terminal states lead to the tour's `failed` step with logs and a retry button. Only steps with `finished` react to run endings. A result step also offers a run with unchanged settings to explore reproducibility. Detailed interpretation in model-backed chat is an optional response to a question, grounded in actual run records and displays.

Progress belongs to one native app (`desktop`) or one Web host (`web:<support directory>`), shared by its windows or tabs. The core persists the first-start answer, demo-offer answer and tour; the live step and run observations last for the service lifetime. A resumed tour uses that live step, or the script's start step after a service restart. Stale choices and repeated or superseded run observations do not advance the guide twice.

The clients draw anchors, position and animate the bird, open panels and perform WKWebView or DOM operations. They perform the reply's actions with the user's controls and report observations to the core. The Web host relays Host requests and changes to its tabs. Model-backed guide conversations use the Agent contract.

[Start a guided tour](Usage.md#working-with-the-agent).

## 10. Demos

[Choose a bundled demo and try a change](Demos.md). Use [Open a Copy](Usage.md#creating-and-copying-projects) to edit it.

### Automatic Skills and project trust

The built-in `helpers` Skill applies without `$helpers`. It says to delegate only
when the person asks, how to scope independent work and what a sub-agent returns.
A same-name override inherits automatic application. Other Skills can declare
`always: true` in their front matter to apply without being named.

The first opening on a machine asks “Do you trust this project?” once. The host
remembers the canonical path and the project folder’s device and inode, shared
by Mac and Web. Replacing the folder at that path asks again; ordinary saves,
including atomic replacement of `tsugumi.json`, keep the answer. The project’s
General settings can change it. Projects created here and copies of
bundled demos are trusted automatically. An untrusted project still opens for
reading; its Skills cannot override built-ins or apply automatically, while a
Skill explicitly selected by `$name` remains available with normal Read access.
Trust concerns Skill instructions; it grants no Card read, write or run access.

Each spawned sub-agent receives `depth` and `count`, defaulting to 0/0. A leaf
cannot spawn. Otherwise it can spawn at most `count` children over its lifetime,
with each child’s depth strictly lower and its exact read/write/run Card sets
contained in the parent’s corresponding sets. The main Agent has no such cap.
The host records consumption across retries and checkpoints. Unscoped helpers
still require the separate permission setting. Nested tasks appear under their
parent in the host-authored conversation presentation.

### Agent project permissions

Agent read, write, run and shell grants apply to the project across all conversations
and helpers until revoked. Attached-folder read grants belong to that project too.
Settings → API → Agent project permissions lists every registered project and its
current grants; each permission can be granted or revoked there. The host validates
and stores these decisions. Opening or deleting a conversation does not change them.
Individual operation approvals and a helper's assigned Card scope remain separate.
Old host state is not migrated before v1.0; grant access again for each project.
