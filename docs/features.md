# Tsugumi's features

Tsugumi is a macOS app that runs graphs of Cards connected by ports (Workflows) in environments pinned with Nix, and records which Card did what and when (there is also a [Web UI](WebUI.md) with the same screens in a browser).
This document is a reference list of what each feature can do and how it is achieved.

| To know | Read |
| --- | --- |
| Why it takes this shape | [Rationale](Rationale.md) |
| Installing and building | [INSTALL.md](INSTALL.md) |
| The list of tests and measurements | [Development.md](Development.md) |
| Isolation and secrets | [security.md](security.md) |
| Plans and the scope of the guarantees | [Limitations.md](Limitations.md) |

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
- **Web UI**: the same screens from a browser. With the Mac's `tsugumi` command it runs inside the same shared VM as the app; on Linux and when started from the repository, `tsugumid` runs directly on the host (Cards are isolated with Seatbelt on a Mac and bubblewrap on Linux; [Web UI](WebUI.md)).

## 2. Cards and Workflows

### Kinds of Card

| Kind | Role |
| --- | --- |
| Code Card | Defines its environment in `default.nix` and its processing in `src/` (C++ / Python / bash / R / Julia / Ruby) |
| trigger | Where a Workflow's run starts (output port `out`) |
| input | Sends a value following the type in `_outputs.fbs`, edited directly on the Canvas (for a File type, the chosen file is copied into the run) |
| fork / branch / merge | Distribution, conditional branching (only one of `a` / `b` fires, depending on `condition`) and merging (a union type named by input port). There is no dedicated Card for bounded feedback loops: a loop is made by sending a value back through a merge, and the `maxRuns` of the Cards on the loop bounds its iterations |
| subgraph (Card in Card) | Handles a child Workflow as one Card. `boundary` maps the outer ports to Cards inside |
| Draft | An "unimplemented Card": write its name and content directly on the card, and connect it just by dropping a wire on it (its ports follow the wires and have no types). The Agent can later generate its implementation |

### Typed ports

- An output port carries one wire. To send one value to several Cards it goes through a fork Card (so there is one way to write it, and the distribution is visible on the Canvas as a Card). An output with two or more wires is reported as `fan_out` and cannot run. Drawing another wire from an output already in use on the Canvas inserts a fork automatically (if the output is a fork or connects to one, that fork gets another branch).
- An output port's type is declared in a FlatBuffers schema (`_outputs.fbs`), and the type of each downstream input port is derived automatically from upstream. Structural compatibility is checked when wiring, and a merge builds a union type automatically.
- Values flow between Cards as FlatBuffers bytes. Automated tests check that 64-bit integers, enums, structs, unions and File / Directory pass between C++ / Python / bash (and R and Julia through an in-memory JSON adapter) without loss.

### Execution semantics (scheduler)

- Inputs are awaited in one of two ways: `all` (consume one value from each once all required inputs have arrived) and `any` (consume them in order of arrival).
- Bounded queues with backpressure, parallelism from 1 to 64, cancellation, a deadline (one for the whole invocation) and an iteration limit (`maxRuns`).
- Downstream of a path a branch did not take is `skipped`; a Card that depends on a Draft or a failed upstream Card is `blocked`, with a reason; paths independent of them keep running.
- A run that was in progress when the process ended abnormally or restarted is recorded as `interrupted`.
- Partial runs with "Run from Here Downstream" or "Run Only This Card (Last Inputs)" reuse the inputs saved last time (including the contents of Files). When "Run from Here Downstream" reaches a merge, a Card that waits for all its inputs fills only the ports that receive nothing this time with saved inputs (an `any` Card runs with only the values that arrived).

### Graph previews on the canvas

- Plotly and Vega-Lite results appear as cached images on the canvas; Expand opens their full interactive viewer. The saved figure data is unchanged.
- Web previews are generated only for visible frames, one at a time in one sandboxed viewer. Completed previews use a bounded memory cache keyed by the full figure content and preview dimensions; closing or hiding a frame cancels its pending work, and an idle renderer is released. A changed result shows a loading state until its own preview is ready.
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

- `pkgs` is by default the bundled, pinned nixpkgs (`tsugumi:default`). Any nixpkgs can also be pinned with its revision and hash, in the form `https://github.com/NixOS/nixpkgs/archive/<rev>.tar.gz#sha256-…`.
- Nix evaluation uses restricted eval and forbids IFD (Import From Derivation), and the URLs it may fetch are limited to nixpkgs archives.
- Tsugumi has no build cache of its own. It relies solely on Nix's own rule, "if the output path of the evaluated derivation exists in `/nix/store`, do not build", and applies it consistently.

### Referring to upstream definitions (`inputs.upstream`)

- From `default.nix`, a Card can refer to the definitions (the functions and values of `run`, `preRun` and `exports`) of the upstream Cards that **always run first, whichever path fires it**.
- Whether a Card always runs first is decided by static analysis that takes into account `all` / `any`, multiple paths, loops (a greatest fixed point) and nesting. A Card on only one side of a branch, a downstream Card or the Card itself cannot be referred to, and such a reference is a static diagnostic (`nix_reference_*`). `inputs.local.<id>` refers only to Cards at the same level.
- The static diagnostics tell you about references as written, before evaluation (they do not read comments or the bodies of strings; an indirect reference through an alias or the like is found at evaluation). The extent that can be referred to is itself guaranteed by the evaluator passing only the Cards that always run first in `inputs.upstream`.
- Only the definitions actually referred to are evaluated, lazily, and branches not chosen at run time are not built.

- On the macOS and Web canvas, **Nix references (as written)** toggles dashed directional links from the referencing Card to the referenced Card. They are the references written in the saved `default.nix` files, not the evaluated dependencies. Hover highlights the connected Cards and links. Nested targets meet their visible container; the reference list preserves the exact paths, opens either Card on the canvas, and opens the referencing Nix definition. Invalid references remain in the list with a diagnostic notice. Saving a source refreshes the shared core resolution; aliases and dynamically computed references are not inferred.

### External Nix sources (`inputs.sources`)

- Flakes can be added under "External sources" in "Name and environment" of the project's settings. Enter an address (`github:owner/repo`, or the URL of a GitHub page) and press "Fetch" to see the commit, the hash and the list of packages for this environment. "Update" fetches a newer commit.
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
- A declaration applies only to that project's Cards (a Card in Card is another project). An external source whose name is not written in a `default.nix` is neither evaluated nor fetched.

### The built-in evaluator

- `tsugumid` embeds `libnixexpr` / `libnixstore` in its process and reuses the loading of nixpkgs, the evaluation state and the daemon connection across Cards and across runs. The preparation stages from the second run on take about 20 ms for 9 Cards in all ([measurements in Development](Development.md)).
- On cancellation or `buildTimeoutSeconds`, only the evaluator's thread is stopped, safely.

### Keeping build results

- All projects share one `/nix/store`, and each project keeps its build results with its own GC roots (the last two generations per Card; ones unused for 30 days are collected automatically).
- A project's "Delete the build cache" only removes that project's GC roots. Garbage collection of the shared store itself runs only from "Clean shared store" under "External Connections" in the app's settings. With `keep-outputs = true`, compilers and the nixpkgs source stay after cleaning, so editing a Card does not download them again.

## 4. Card SDK and workers

### Stages

```text
waiting for inputs → SDK preparation → Nix evaluation → build → preRun → run → postRun → output validation → saving → done
```

- Each stage's start and end times and its result (done, cache used, reused, failed, stopped) are recorded in `InvocationSummary.stages` and returned both in real-time notifications and in the history. The UI draws the stage display and the highlighting of wires from this record alone, never guessing from the final state.
- `preRun` prepares before the run, and `postRun` collects artifacts and cleans up (it runs even after `run` has failed, with `TSUGUMI_FAILED=1`). After cancellation or a deadline, nothing further starts, `postRun` included.

### Language SDKs

- **C++**: with the generated header `card.hh`, port names become typed members as they are.
  ```cpp
  auto run(Card::Input& input) -> Card::Output {
    Card::Output out;
    out.next.emplace().value = input.number.value / 2;
    return out;
  }
  ```
  The worker loads the built shared library `lib/main.so` into memory and calls it directly (no process is started per invocation).
- **Python**: implement `main(ctx)` and, optionally, the initialisation hook `on_run_start(ctx)`.
- **bash**: inputs arrive per port as `JSON_P` (the path of a JSON file) / `FILE_P` / `DIR_P`; the JSON written to `$TSUGUMI_OUTPUT` is the output, type-checked per port.
- **Ruby** (`rubyCard`): MRI runs in the package worker with typed Hash/Array values, exact 64-bit integers, live displays and optional resident state. The Card can name a Gemfile.lock and the hash of its gem files to pin its dependencies at build time. [The Card guide](../agent/Reference/card-guide.md#ruby-srcmainrb) describes the API.
- **R and Julia** (`rCard`, `juliaCard`): their interpreters live in the same ABI worker as C++/Python. An invocation starts a new interpreter unless `reuseWorker` is on, which keeps the interpreter and its module state; displays publish during execution. The list/Dict interface uses in-memory JSON conversion against each port’s FlatBuffers schema, without per-call processes or JSON files. How to write them is in [Your first Card](CardGuide.md).
- What does not depend on the port contract (visualisation, logs, the run context) is gathered in the common runtime `libtsugumi_card.so`, so all that is built per Card is the type definitions and a thin adapter.

### Resident workers

- By default a Card keeps no state: `reuseWorker` is `false`, and every invocation gets a new worker (a new process, a new interpreter).
- A worker of a Card with `reuseWorker = true` stays resident across invocations and runs and keeps its state, static variables included (`onRunStart` / `on_run_start` is called before the first invocation of each run). Each invocation records whether its worker was reused.
- It ends automatically when its build conditions change, on failure or cancellation, after 10 minutes unused, above 2 GiB of memory, or when more than 32 are resident.
- For a Card without `reuseWorker`, the new worker for next time is started ahead in the background (sandbox started, `.so` loaded, Python imports done, the R or Julia source loaded) and kept waiting.

### Another system (x86_64-linux)

- A Card can be built and run for another Nix system: "System" in its settings (`"system": "x86_64-linux"` in `_card.json`), for software with no aarch64 build. nixpkgs is imported for that system, and the kernel runs its binaries with an emulator registered with binfmt_misc.
  - **Mac (the shared VM):** Rosetta when it is installed on the Mac. qemu (`qemu-user` from cache.nixos.org) only when asked for with `TSUGUMI_VM_X86=qemu`, for a Mac without Rosetta or to try the qemu path. The app never installs Rosetta itself.
  - **Linux:** whatever the system registered, such as `boot.binfmt.emulatedSystems` on NixOS (with Nix's `extra-platforms`).
- Each invocation records the emulator that ran it (its path; qemu's includes its version), shown in the Info tab of the Card's run history. Rosetta and qemu need not give the same numbers, so the record tells them apart.
- Every language works. bash Cards are programs the kernel hands to the emulator. C++, Python, R and Julia Cards are a `main.so` the worker loads, so their build also makes a worker for that system (`worker` of the Nix SDK) from the Card's own nixpkgs, and the kernel runs it under the emulator: the worker and the Card share one glibc, libstdc++ and libpython. It is built once per nixpkgs and system, under emulation the first time (on an M3 Max: the first C++ Card's build stage took 2 min 16 s with Rosetta, the x86_64 compiler's download included, and the worker alone 4 min with qemu), and reused from the store after that by every such Card, whichever emulator built it.
- Without an emulator, the run stops before building, saying what is missing.

### Outside access (Experimental)

- By default a Card has no network and cannot see the Mac's file system ([security.md](security.md)).
- In the Experimental section of a Card's settings, "Allow network access" (`network`) and "Allow access to folders of the host" (`hostFolders`; seen from the worker as `/host/<name>`, read-only by default) can be set per Card.

## 5. Documents and saving

- A project is an ordinary folder of `tsugumi.json`, `definitions/<Card>/` (`_card.json`, `_meta.json` (intentions such as the summary), `default.nix`, `src/`, `_outputs.fbs`) and the layout in `layout.json`, and can be managed with Git as it is.
- Every JSON file is validated strictly against a typed schema (UTF-8; duplicate keys, non-finite numbers and unknown keys refused) before use. The core does not re-encode the document text it receives, so the notation of 64-bit integers and the like is kept as it is.
- **Save transactions**: apply provisionally in memory and validate → `fsync` a journal (the contents of all files and a receipt) → replace the target files atomically → commit. Should it crash in the middle, the journal is applied again at the next start, so the result is always "all applied" or "none applied" (tested with fault injection).
- **Conflict detection**: a save is based on the revision at which editing began. If another window or the Agent saved first, it is detected as a conflict and nothing is silently overwritten. A resend with the same `op_id` is applied only once (if a response is lost to a dropped connection, `OperationGet` confirms the result).
- **Saving during a run**: while a Workflow runs, a save that changes the computation's definition (its execution fingerprint) is refused with `busy`; only layout changes and the like are accepted, safely.

## 6. Run history and artifacts

- Runs, invocations, stages, input and output values, logs, visualisation data and receipts are all recorded in SQLite. The primary copy is on the VM's dedicated run-history disk (`vm/history.img`), so it is written fast without going through a folder shared with the host, and is not lost by "Reset all environments".
- A Card's run history shows, for each invocation, the **packages** it ran with: the runtime dependencies recorded for that invocation, with search and pagination. They are read from the history, never derived from the current Nix definition, so an edited definition does not change what an earlier invocation shows; an invocation without a recorded environment says so. Build-only dependencies are not included.
- `recordIO` and `retainArtifact` choose, per Card, what is recorded and kept. Kept artifacts are checked for tampering and path escapes.
- "Compare Runs" in a Card's run history or the bottom run panel opens two runs side by side on Mac and Web. Each Card's last invocation shows its state, duration, recorded parameters, inputs and outputs, and retained table, chart or image. Changed fields and displays are marked; missing, failed and stopped Cards remain visible. The same screen shows a direct code/settings diff between the runs' versions without checking either version out. Comparison only reads the records and never replaces the current Canvas's results.
- With `saveRunHistory` (default `true`), the history of finished runs is also written automatically to `.tsugumi/runs.sqlite` in the project. Input and output values are kept as FlatBuffers bytes with their schema (bfbs) retrievable by hash, so past values are interpreted correctly even after a Card's types change.
- When a project is copied or moved to another Mac, its history is imported automatically on opening, matched by `run_id`.
- "Delete Run History" deletes from all three places: inside the VM, `.tsugumi/runs.sqlite` and the per-run file area.

## 7. Visualisation

- Calling APIs such as `tsugumi::visualization::text(...)` from a Card's code displays text, scalars, line charts, tables, images, 3D point clouds and Canvas modules.
- Line charts are drawn fast with Dawn (a WebGPU implementation, Metal backend) and update in real time during a run, as in the Live Stream demo.
- matplotlib in a Python Card draws straight into the Card's display through the SDK's backend (`tsugumi.mpl`): code written for a window runs as it is, like X forwarding. `plt.show()` shows a figure (figures still open are shown at the end of the invocation), and loops with `plt.pause()` and `FuncAnimation` update live during the run. Cards that use matplotlib get a Japanese font (IPAex), so Japanese labels draw as they are.
- Plotly's `fig.show()` and Altair's `chart.show()` appear in the Card's display as interactive charts (zoom, hover and legend operations in the enlarged view). They are drawn with the bundled plotly.js / Vega and never go to the network (a Card's charts cannot read data from outside). See the Plots demo.
- A display is a separate frame on the Canvas that can be placed and enlarged freely, and its layout is saved in `layout.json`. Renaming a Card moves its position, frame and group to the new name, and removing it from the Canvas removes them with it (the same whether from the UI or the Agent, through `tsugumid`'s save).

## 8. UI (the workbench)

- The sidebar on the left: "Workflow Hierarchy", "Add Card" (definitions of imported projects, and definitions not placed) and "Cards on Canvas". At the bottom, buttons for the version history and the project's settings.
- The Canvas in the centre: one floating row at the top with breadcrumbs through the hierarchy, the run operations, "Generate Cards", "Generate from comments", and buttons for going back Home and for the chat. Below, zoom and arrangement controls and a status bar, and under them the collapsible log and timeline panels.
- On the right: the chat is the right-hand column of the workbench; its width can be changed and it can be a separate window.
- Canvas operations: adding and moving Cards (snapping to a grid), duplicating, wiring, range selection, zoom, arranging (a layered layout with straight wires, in which Cards, output frames and sticky notes do not overlap, undone in one step), groups, Markdown sticky notes and display frames.
- "Untangle Wires" reduces crossings by reordering the visible ports while keeping Card positions. Arrange also runs this step after placing Cards. Both operations undo in one step. Port names, types, connections and execution stay unchanged; only the visual order is saved in `layout.json`. The pass considers routed wires, output-frame obstacles and child Workflow boundary wires, and keeps a proposed order only when it reduces crossings. Routing Cards retain their shape's port order; some graphs still have unavoidable crossings.
- The Card settings window: General, Comments, Nix Definition, Source (syntax highlighting, automatic formatting, showing where errors are), Ports, Run History, Artifacts and Additional Context. Edits are made on a working copy and put in with "Apply".
- The settings window: the whole app (General, Appearance, Editor, Shortcuts, Skills, API, Connections) and the open project (Name and environment, Canvas, Run history, Build cache). Each item shows where its value is kept.
- Custom signed [binary caches](BinaryCaches.md) (Attic, Cachix and compatible servers), configured for the Mac shared execution environment, including private-cache authentication. Changes apply at the next VM start.
- A terminal inside the VM for each project (bash started in that project's namespaces with its own uid). The Mac panel has independent tabs, up to eight per project as in the Web UI. The tabs and their shells belong to the open project, not to the panel: opening or selecting another tab, hiding the panel and showing it again all keep existing commands and scrollback, while closing a tab or closing the project hangs up its shells. Opening the panel before the environment connects waits and connects automatically; after a reconnection, tabs whose shell had ended get a new one. When two windows show the same project, they share the same tabs, and a terminal is drawn in the window that showed it last.
- Japanese or English, chosen under "General" in the settings (Japanese by default, whatever the system's language; it takes effect when the app restarts, or by reloading the page in the Web UI). The Mac app and the Web UI on the same Mac share it, and what tsugumid writes into a project's git for a run follows it ([Versions](Versions.md#the-results-of-runs)).
- While running, changes to Cards and closing the project are blocked; progress, history and artifacts can still be seen.

### Several projects

- The app shows one project (or Home) per window. ⌘⇧N adds a Home window, and ⌘N, ⌘O, demos and duplicates from a project's window open in a new window. A project already open has its window brought to the front.
- ⌘W saves and closes only that project (not while it runs), and ⌘Q asks about the unsaved changes of all projects at once.
- What is separate per project is `tsugumid` (document, runs, history), the chat and Agent, and tabs and selections. The shared VM, `/nix/store`, CPU and memory, API keys and app settings, and Home's guide conversation are shared. Closing one leaves the other running.
- The Web UI has one per browser tab ([Several projects in the Web UI](WebUI.md#several-projects)).

## 9. Agent

A separate, independent package, `agent/`. The Agent is one client using the same RPC as the UI; without the Agent, making, editing and running Cards can all be done on screen.

### Commands in the project environment

The Agent's `project_shell` tool uses the public `ProjectShell` RPC, enabled only by the isolated project launcher. It runs as the project's user in `/project`, never as a fallback shell on the Mac or Web host. Agent use needs separate human-granted **Shell** access in addition to Read, Write and Run. Shell access includes the project's shared folders and the VM network; it is broader than an individual Card's permissions.

Commands and tool results remain in the conversation, with exit status, signal, timeout and cancellation. Each stream is bounded to 256 KiB and reports truncation; the default deadline is 10 seconds and the maximum is 30 seconds. Disconnecting cancels the process group. An operation ID is durable and cannot run twice after a retry or restart; an interrupted operation with an unknown outcome must be inspected before issuing a new one. Commands that edit project documents need `ProjectReload` before subsequent inspection or execution. These commands are recorded as tool operations, not fabricated Card invocations.

The ordinary Web host daemon does not enable this tool. Web clients connected to the Mac VM can use the same RPC and approval boundary. The Web UI's human terminal is distinct from the Agent tool: with the Mac VM it is the project's shell in the environment, as in the app, and on an ordinary Web host it is a shell of that host in the project's folder.

### Choosing Claude's features by purpose

Tsugumi uses Claude's models and features by choosing the one that fits each job. Each row's "Where to check" is code and tests.

| Purpose | Feature used | Where to check |
| --- | --- | --- |
| Make, fix, run and check Cards | Tool use in the Messages API. The tools are generated from the RPC contract (`protocol/tsugumi.fbs`), so the Agent has exactly the operations of the screen | [Tools and the RPC](#tools-and-the-rpc), `test_agent_contract.py` |
| A model sized to the job | Implementing Cards uses the model the user chooses (default `claude-sonnet-5-5`, with effort and Fast mode selectable). The guide's conversation, the summaries of the thinking of the bird on the Canvas and the version summaries use the small `claude-haiku-5-5` | [Connecting to models](#connecting-to-models), `modelOptions` in `agent/Shared/agent.json`, [Version summaries](Versions.md) |
| Judging from the figures a run makes (multimodal) | `run_record` passes the images a Card displayed (charts, RGB images and so on) to the model as images. The same in the app, the Web UI and the Claude Code plugin | `agent/Sources/TsugumiAgent/Provider.swift`, `web/server/agent.ts`, `web/server/mcp.ts`, `ToolImageTests` in `macOS/CoreTests` |
| Implementing independent Drafts at the same time (subagents) | `delegate` runs 2 to 8 helpers at once, splits the Cards among them, and finally merges them into one comparison | [Splitting the work](#splitting-the-work-delegate), `TeamTests` in `macOS/CoreTests` |
| Guiding through a fixed procedure (Skills) | The built-in Skill `$demo` (`agent/Shared/demo-skill.md`) guides through a demo, and Skills the user adds are loaded too | [Skills and conversations](#skills-and-conversations), [the guide](#the-thrush-chat-and-guide) |
| Showing work in progress | Tool arguments are received by streaming (`eager_input_streaming`), and the Card's code is shown on the Canvas as it is written | [The Agent on the Canvas](#the-agent-on-the-canvas), `BrokenArgumentsTests` |
| Making long work faster and cheaper | Prompt caching on the system prompt, the tool definitions and the conversation | `agent/Sources/TsugumiAgent/Provider.swift` |
| When up-to-date information is needed | The web search server tool (off by default; only when the user turns it on) | [Web search](#web-search), `WebSearchTests` |
| Working with outside agents (MCP) | The Claude Code plugin hands Tsugumi's tools over as an MCP server, and the work appears in the Web UI. A save that widens a Card's permissions waits for a person's approval | [Using it from Claude Code](claude-code.md), `test_claude_code_plugin.py` |
| Guiding without an API key | A scripted guide that uses no model (`agent/Shared/guide-script.json`). Sends nothing anywhere and costs nothing | [The thrush's guide](#the-thrush-chat-and-guide) |

Besides Anthropic's Messages API, OpenAI's API and the ChatGPT plan are supported too ([Connecting to models](#connecting-to-models)).

### Connecting to models

- Supports the Anthropic Messages API and the OpenAI Responses API, responding by streaming. OpenAI can be used with an API key or with the user's ChatGPT plan (Sign in with ChatGPT, the procedure for open-source tools) ([Security](security.md#the-chatgpt-plan-sign-in-with-chatgpt)).
- The chat's model menu chooses effort and Fast mode. Only models and values confirmed in each provider's documentation are offered (`modelOptions` in `agent/Shared/agent.json`). If nothing is chosen, these fields are not sent and the model's defaults apply.

### Tools and the RPC

- The definitions of the tools the Agent uses are generated automatically from `tsugumi.bfbs` (the FlatBuffers schema with its doc comments), and the conversion between tool arguments and FlatBuffers messages uses the same schema. Updating the RPC contract updates the Agent's tools with it.
- The Agent gives each tool call that changes something a unique `op_id`, and it goes through exactly the same revision checks, conflict detection and confirmation notifications as the UI.
- Attach a Mac folder to the chat and the Agent can read it with `AttachedFolderRead` (shared read-only with that project only; what is read is sent to the model).

### Web search

- Turn it on under "API" in the settings and the chat's Agent can search the web at its own discretion. Off by default; while off, the model is not shown the tool. That search terms go outside, and the risk of unpublished data being in them, are in [Security](security.md#web-search).
- Search uses the provider's feature (Anthropic's `web_search`, OpenAI's `web_search`; the same with the ChatGPT plan). Tsugumi does not connect to a search service. The local fixture has no search.
- A search appears in the chat as a row like other tools, so its terms and the URLs found can be followed. Sources the response cites appear as links under it. With the ChatGPT plan, only the cited sources appear (because `include` is not sent).
- Only the chat's Agent can use it; helpers and the guide's thrush do not search.

### Generating Cards from Drafts

- The Draft's summary, processing, additional context, ports and the wires around it are given to the Agent, which saves the types, the Nix definition and the source code in one transaction and then checks them by building and running.
- The order is "write first, run later". The Agent finishes writing as many Cards as it can, then runs once from the trigger to check. It runs in between only when writing the next Card needs information that only a run can give (the columns of a downloaded file, say).
- On failure or cancellation in the middle, half-finished changes are not saved (the plan stays).
- The intention written in a Draft stays as it was. A summary and additional context that were empty are filled in by the Agent from the instructions in the chat and the Draft's content (it adds nothing that was not said). `tsugumid` refuses a save that leaves them empty or changes what was written.

### Generating Cards from comments

Write "I want this changed" on a Card and have the Agent follow it. If a Draft is the plan of a Card before implementation, a comment is an instruction to the Card itself. Any kind of Card can have them.

- Write one with "Comments…" in the Card's context menu, or from the badge at the top right of a Card on the Canvas ("Comments" in the Card's settings). Adding, editing, resolving, reopening and deleting are saved on the spot (ones written during a run are saved when the run ends, like other edits).
- "Generate from comments" on the Canvas hands the unresolved comments to the chat's Agent: only the selected Card's comments if a Card is selected, otherwise all of those at the level shown. The comments handed over appear in the chat.
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
- Each helper has its own conversation and the same tools, and can save only the definitions of the Cards it was given (it cannot change `tsugumi.json`, the layout or other Cards).
- In its first message, a helper receives the definitions and `_meta.json` of its Cards, the wires to them and the upstream Cards that make their values (types and code), as read by the app. It can start without reading the whole project, so the parent's instructions need only say what the Drafts and rules do not.
- The tools and rules common to the helpers are written into the model's cache once, before they start, and all of them read from there, because requests sent at the same time cannot read a cache another is still writing (Anthropic connections only).
- When the helpers' saves and runs collide (`busy` during a run, `revision_conflict`), the Agent's side waits for the run to end and resends from the latest revision.
- How to wait is chosen by the Agent when it splits the work (`runs` of `delegate`). With `each` (the default) each helper runs on its own, and while one run is going the other helpers' saves and runs wait. With `together` the helpers only write and validate (a run is refused), and once all have reported, the Agent runs once. The Agent chooses `together` unless a helper needs the results of its own run to write.
- When all helpers have reported, the Agent checks each one's Cards and reports. The helpers' conversations are kept next to the original conversation (`<conversation>.helpers/`).

### The Agent on the Canvas

While the Agent works, a thrush flies over the Canvas showing what it is doing (it does not take clicks).

- Reading a Card, it perches on that Card; writing a Draft up into a code Card, it draws as if drawing a line; for a new connection it threads a string; running, it flies to the start Card. Its speech bubble says what it is doing now.
- While the Agent writes a file into a save's arguments, the file name and the last few received lines appear next to that Card without simulated typing. `PutSource` shows file content; `PatchSource` shows only the current replacement text, explicitly marked as a patch. The latest file or patch replaces the previous one immediately, and a late path reveals the content already received. On the Mac the bird plays its scenes one at a time, and the scenes of a change that was already saved (writing its Cards out, threading its wires) are not cut for the next file: that file's panel appears once they have played, with everything received by then. Panels say "before saving" until the save succeeds, disappear on failure/cancellation or when a run starts, and follow the Agent visibility setting. They take no clicks and hide below 50% canvas zoom. Helpers each have one panel in their own color.
- When the work is split, a thrush of a different colour is added for each helper, and the parent Agent's thrush is gold, named "Agent".
- Turn on "Draw thoughts" (the button under the input field) and a small model summarises the Agent's messages and tool results into one sentence for the speech bubble (it uses tokens apart from the Agent, so it asks the first time it is turned on).
- Its look is chosen under "The Agent at work" in "Appearance" of the settings: a thrush, a dot or nothing.

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

- The guide has only the reading tools, `reference_read` and `guide_show`. It does not save, run or stop.
- The guide answers with a small model: `claude-haiku-5-5` for Anthropic, the luna model for the ChatGPT plan, and the Agent's model with an OpenAI API key. A demo's guide is carried on by the copy's chat, with the chat's model.
- Conversations are kept in `~/Library/Application Support/Tsugumi/agent/guide/` on a Mac and in `agent/guide/` of the data folder on the Web.
- On first launch, choose Anthropic or OpenAI, enter one API key and confirm the connection before starting ("Skip and start" is also possible). The key is saved in the Keychain by the same route as "API" in the settings. If a usable key (or the ChatGPT plan) already exists, nothing is asked.
- The first time it appears it asks "What would you like to do?" and offers "Look at an existing Workflow" and "Have AI implement Drafts". The former asks for a field (`field` in `demos/en/index.json`) and then a demo in it; the latter asks for a Draft demo. Up to here the app asks with fixed text, so no model is used. It flies to the chosen demo with the demo list open and offers "Open this demo" ("Other demos" chooses again). Only when you write a question or your interests, or press "Ask for a recommendation", does the guide answer with a model, choosing demos that fit by field, description and tags. Once a demo is opened, the thrush of that copy's chat carries on the guide.
- While not connected to a model (no API key and no ChatGPT sign-in), the bird still appears and answers from a fixed script (`agent/Shared/guide-script.json`, the same file read by the Mac and the Web). It sends nothing anywhere and costs nothing.
  - The script can guide through copies of three demos: Data to report, Kepler-6 b and Pharmacokinetics. It starts on opening one, or on sending `$demo` in its chat, and proceeds by following the choices. The results of a run lead on separately for success and failure.
  - In other demos and projects, it offers buttons to open these three. To a free question it replies that connecting Claude (an Anthropic API key) or Codex (an OpenAI API key or ChatGPT sign-in) under "API" in the settings lets it answer, and that `$demo` starts the guide in a demo's copy.
- The first time the user connects to a model by saving a key (or signing in to ChatGPT), the main window in front asks once "Would you like to see the guide?", noting that the guide uses the model's API and costs money. "See the guide" opens the Data to report demo and starts the guide; "Later" is the same as "Later" in the speech bubble.
- During the guide, the start and end of runs (succeeded, failed, stopped) and disconnections and reconnections are reported to the chat as they really are. What happens while it is answering is reported after it finishes. The guide's state and the answer to the invitation are saved and not repeated after a restart.

## 10. Demos

There are 31 bundled demos (Draft versions included), in a Japanese set and an English set (`demos/ja`, `demos/en`): the same demos and files, their descriptions, notes and the text of their figures in one language. The app and the Web UI list the set of their language. What you can try, and how each demo's Cards connect and behave, is in [The bundled demos](Demos.md).
Choose "Open a Copy" on a demo from "Browse Demos" on the home screen to get an editable copy in `~/Documents/Tsugumi/<demo>`.
