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
- **Pin dependencies and control state.** Use **Project settings → Name and Environment** to pin Nixpkgs or add a trusted flake. Declare a Card-local lock where the SDK supports one (Python, Ruby, Go, Rust and C#). For a Card that needs interpreter or library state, turn on **Keep the worker's state** (`reuseWorker`) in its settings. [Nix environments](#3-reproducible-environments-with-nix) · [Language SDKs](CardGuide.md) · [Worker state](CardGuide.md#worker-state)
- **Use a shell or a signed build cache.** Open the project Terminal to run a tool such as `nix --version`. On Mac, configure a shared signed cache in **Settings → Binary Caches** and apply it on the next VM start. [Project terminal](Usage.md#project-terminal) · [Binary caches](BinaryCaches.md) · [Browser and CLI platforms](WebUI.md#the-tsugumi-command)
- **Run a Card on another Linux machine.** Start `tsugumi-remote-worker` there, register it in **Settings → Execution service**, and choose it under **Execution service** in the Card's settings. [Run a Card on a remote worker](RemoteExecution.md) · [Execution service](#execution-service)
- **Control a Card's access.** Use **Card settings → General** to review **Restrict file access**, and the network and host folder permissions under **Experimental**, before running code from another project. [Security and isolation](security.md#summary)

### Work with the Agent and explore examples

- **Turn a Draft into a Card.** Connect a model under **Settings → API**, describe an operation in a Draft and choose **Generate Cards**. Review the generated source, ports and run record. Use Card comments to request a specific change. [Generate from Drafts](#generating-cards-from-drafts) · [Generate from comments](#generating-cards-from-comments) · [Data and permissions](security.md#8-the-models-api-keys-agent)
- **Start from an example.** Browse Demos and open a copy to change a parameter, run it again, or follow the thrush's guide. Use the [documentation site](https://gen740.github.io/Tsugumi/)'s recordings to inspect example Workflows before installing. [Bundled demos](Demos.md)
- **Work from Claude Code.** Install the plugin to have Claude Code make and run Cards through Tsugumi's tools, with the work shown in the Web UI. [Claude Code plugin](claude-code.md)

## 1. Architecture

See the [trust boundaries](security.md#1-the-trust-boundaries), [RPC contract](Protocol.md) and [repository map](../AGENTS.md#map).

The host-side `tsugumi-agentd` exposes environment operations through `EnvironmentRequest` in `protocol/host.fbs`: state, start, stop and attached-folder changes. Its native adapter launches `tsugumid` on Linux; its signed Mac adapter boots the shared VM and applies the mounts agentd specifies. In both cases, the service owns readiness checks, endpoint and project registration, execution-broker authorization, concurrent starts and process-exit reporting. Host events carry state, progress and stopped notifications. Environments remain resident when clients disconnect or agentd exits. Archiving incompatible history requires an explicit start flag. `HostRuntimeRequest` reports runtime/store state without starting a VM.

The Mac app, Web host and `tsugumi` command use the same Host environment requests and events. `tsugumi-vmhost` exposes the private length-framed FlatBuffers contract in `protocol/vm_host.fbs` to agentd. It retains Virtualization.framework, virtiofs/vsock and terminal descriptor bridges because they need the app’s signature and entitlement. Clients display runtime snapshots and exchange terminal bytes through Host requests; they do not connect to the helper. Store cleanup, history deletion and reset are explicit Host runtime actions.

## 2. Cards and Workflows

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
| Draft | An "unimplemented Card": write its name and content directly on the card, and connect it just by dropping a wire on it (its ports follow the wires; they are typed only if the Draft has an `_outputs.fbs`). Wire-created inputs live in `_card.json`, outputs in `_meta.json`, and disappear when the last wire is removed. The Agent can later generate its implementation |

A Card in Card uses full Card paths to distinguish state and history; visual groups in `layout.json` do not affect execution.

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

### Keeping build results

Use [Build Cache settings](Usage.md#manage-build-and-scratch-space) to inspect and explicitly clear build registrations and store space. [Binary caches](BinaryCaches.md) explains shared caches.

## 4. Card SDK and workers

Start with [Your first Card](CardGuide.md), then use the [SDK reference](../agent/Reference/card-guide.md), [worker state rules](CardGuide.md#worker-state) or [C# guide](CSharp.md). Inspect execution stages in [Run History](RunHistory.md#inspect-an-invocation).

### Execution service

[Register a remote worker](RemoteExecution.md) to build and run a Card on another Linux host. Review the [data boundary](security.md#remote-worker-boundary) before sending inputs.

### Another system (x86_64-linux)

- A Card can be built and run for another Nix system using **CPU architecture** in its settings (`"system": "x86_64-linux"` in `_card.json`), for software with no aarch64 build. The value is `x86_64-linux` or `aarch64-linux`. For the local service the list offers the native system and each system with a registered emulator, and for a registered remote worker the systems that worker reports. nixpkgs is imported for that system, and the kernel runs its binaries with an emulator registered with binfmt_misc.
  - **Mac (the shared VM):** Rosetta when it is installed on the Mac. qemu (`qemu-user` from cache.nixos.org) only when asked for with `TSUGUMI_VM_X86=qemu`, for a Mac without Rosetta or to try the qemu path. The app never installs Rosetta itself.
  - **Linux:** whatever the system registered, such as `boot.binfmt.emulatedSystems` on NixOS (with Nix's `extra-platforms`).
- Each invocation records the emulator that ran it (its path; qemu's includes its version), shown in the Info tab of the Card's run history. Rosetta and qemu need not give the same numbers, so the record tells them apart.
- The kernel runs Bash Cards' programs under the emulator. Package Cards load a `main.so`, so their build also makes a worker for the selected system (`worker` of the Nix SDK) from the Card's own nixpkgs, and the kernel runs it under the emulator. The worker and Card use the same target runtime libraries. It is built once per nixpkgs and system, under emulation the first time, and reused from the store after that by every such Card, whichever emulator built it.
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

Connect a model under **Settings → API**, then ask the chat to make or change Cards. [Permissions and sent data](security.md#8-the-models-api-keys-agent) · [Agent controls](Usage.md#working-with-the-agent)

### Commands in the project environment

Grant **Shell** access separately from Read, Write and Run to use `project_shell`. It runs inside the isolated project environment and includes its shared folders and network; it is broader than one Card's permissions. [Shell contract and limits](Protocol.md#projectshell) · [Human terminal](Usage.md#project-terminal)

### Choosing Claude's features by purpose

Tsugumi uses Claude's models and features by choosing the one that fits each job. Each row's "Where to check" is code and tests.

| Purpose | Feature used | Where to check |
| --- | --- | --- |
| Make, fix, run and check Cards | Tool use in the Messages API. The tools are generated from the RPC contract (`protocol/tsugumi.fbs`), so the Agent has exactly the operations of the screen | [Tools and the RPC](#tools-and-the-rpc), `core/src/agent/catalog.cc`, `tests/core/agent/` |
| A model sized to the job | Implementing Cards uses the model the user chooses (default `claude-sonnet-5-5`, with effort and, on models that offer it, Fast mode selectable). With an Anthropic API key, the guide's conversation, the summaries of the thinking of the bird on the Canvas and the version summaries use the small `claude-haiku-5-5` (other providers use a small model of their own or the Agent's model) | [Connecting to models](#connecting-to-models), `modelOptions` in `agent/Shared/agent.json`, [Version summaries](Versions.md) |
| Judging from the figures a run makes (multimodal) | `run_record` passes the images a Card displayed (charts, RGB images and so on) to the model as images. The same in the app, the Web UI and the Claude Code plugin | `core/src/agent/tool_images.cc`, `web/server/mcp.ts`, `tests/core/agent/project_rpc_test.cc` |
| Implementing independent Drafts at the same time (subagents) | `delegate` takes 1 to 8 tasks, runs up to 4 helpers at a time, and returns their messages and errors | [Splitting the work](#splitting-the-work-delegate), `tests/core/agent/project_rpc_test.cc` |
| Guiding through a fixed procedure (Skills) | The built-in Skill `$demo` (`agent/Shared/demo-skill.md`) guides through a demo, and Skills the user adds are loaded too | [Skills and conversations](#skills-and-conversations), [the guide](#the-thrush-chat-and-guide) |
| Showing work in progress | Tool arguments are received by streaming (`eager_input_streaming`), and the Card's code is shown on the Canvas as it is written | [The Agent on the Canvas](#the-agent-on-the-canvas), `core/src/agent/provider.cc` |
| Making long work faster and cheaper | Prompt caching on the system prompt and the tool definitions | `core/src/agent/provider.cc` |
| When up-to-date information is needed | The web search server tool (off by default; only when the user turns it on) | [Web search](#web-search) |
| Working with outside agents (MCP) | The Claude Code plugin hands Tsugumi's tools over as an MCP server, and the work appears in the Web UI. A save that widens a Card's permissions waits for a person's approval | [Using it from Claude Code](claude-code.md), `test_claude_code_plugin.py` |
| Guiding without an API key | A scripted guide that uses no model (`agent/Shared/guide-script.json`). Sends nothing anywhere and costs nothing | [The thrush's guide](#the-thrush-chat-and-guide) |

Besides Anthropic's Messages API, OpenAI's API and the ChatGPT plan are supported too ([Connecting to models](#connecting-to-models)).

### Connecting to models

The host-side `tsugumi-agentd` owns provider endpoints, revisioned settings and OAuth credentials. The clients keep settings screens, presentation snapshots, browser opening and macOS login Keychain access. Keychain-backed API keys are provisioned to agentd in memory only; non-Mac and Web Keychain-save fallback keys use its owner-only store. Host RPC supplies settings get/update, secret-input provider set/status and sign-in begin/action/status; ordinary model selections contain no endpoint or credential. Desktop settings are shared; the app and Web hosts have separate ChatGPT account profiles. OAuth validation, callback handling, refresh rotation and revocation run in core. See [API keys and sign-in](security.md#8-the-models-api-keys-agent).

- Supports the Anthropic Messages API and the OpenAI Responses API, responding by streaming. OpenAI can be used with an API key or with the user's ChatGPT plan (Sign in with ChatGPT, the procedure for open-source tools) ([Security](security.md#the-chatgpt-plan-sign-in-with-chatgpt)).
- The chat's model menu chooses effort and Fast mode. The model list comes from the provider; the effort levels and Fast mode offered for a model are only those confirmed in the provider's documentation (`modelOptions` in `agent/Shared/agent.json`). If nothing is chosen, these fields are not sent and the model's defaults apply.

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

### The Agent on the Canvas

[Agent activity and Draw thoughts](Usage.md#agent-activity).

### Skills and conversations

The host-side `tsugumi-agentd` discovers Skills, parses their metadata and reports unreadable or oversized files through `SkillsListRequest` in `protocol/host.fbs`. The app and Web host keep folder selection, completion and menus; they send selected Skill names. The service resolves every matching name in source order and reads the contents beneath registered host roots only after Read access is granted. A Skill must be smaller than 512 KiB. Discovery does not grant access to use its contents in a model prompt.

`ModelsListRequest` reads or refreshes the service's persisted catalog for each connection. Both clients use its `name` display field. An empty refresh leaves the stored catalog intact; a failed refresh does not change it. Clients retain only a presentation snapshot for rendering an open menu.

[Skills and conversations](Usage.md#skills-and-conversations).

### The thrush (chat and guide)

The host-side `tsugumi-agentd` embeds and interprets `agent/Shared/guide-script{,.en}.json`. `GuideGetRequest` reads a session in Japanese or English; `GuideAdvanceRequest` reports an interest, a message or choice, a demo opening, a run observation, or a completed client action. The reply contains the step, text, choices, semantic actions, highlight and panel request. Scripted guidance works without a model connection.

Progress belongs to one native app (`desktop`) or one Web host (`web:<support directory>`), shared by its windows or tabs. The core persists the first-start answer, demo-offer answer and tour; the live step and run observations last for the service lifetime. A resumed tour uses that live step, or the script's resume step after a service restart. Stale choices and repeated or superseded run observations do not advance the guide twice.

The clients draw anchors, position and animate the bird, open panels and perform WKWebView or DOM operations. They perform the reply's actions with the user's controls and report observations to the core. The Web host relays Host requests and changes to its tabs. Model-backed guide conversations use the Agent contract.

[Start a guided tour](Usage.md#working-with-the-agent).

## 10. Demos

[Choose a bundled demo and try a change](Demos.md). Use [Open a Copy](Usage.md#creating-and-copying-projects) to edit it.
