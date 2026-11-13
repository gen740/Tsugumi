# Development

Tsugumi's runtime, design decisions, builds and test commands.
Installing and building the app are in [INSTALL.md](INSTALL.md), the pinned versions of dependencies in [Lock.md](Lock.md), and the overview for users in the [README](../README.md).
File paths and commands on this page are relative to the root of the repository.

## The runtime

The types of ports check that wiring is consistent, and Nix pins each Card's environment by hash.
The run history and artifacts are persisted, and each Card's input and output values are recorded as configured.

In the macOS app, computation runs in an isolated Linux VM on the Mac, and Cards' access to the outside network is off by default. The Web UI uses the same VM on a Mac when started with the installed `tsugumi` command. The repository's development setup can run `tsugumid` directly on a Mac with Seatbelt isolation ([where the Web UI runs](WebUI.md)).
On the other hand, the Agent, which uses an outside LLM, is sent the conversation and what its tools read (source code, logs, input and output values and so on), so the isolation of the computing environment and the extent of data sent to outside models must be managed as separate things. The details are in [Security and isolation](security.md#what-is-sent-to-the-model).

The graph execution engine supports conditional branches (branch), merges (merge), bounded feedback loops and nested child Workflows (Card in Card).
In the macOS app, one shared Linux VM for the whole app (using `Virtualization.framework` directly) runs a separate `tsugumid` process for each open project.
A main window shows one project, and several windows can be open at once ([Several projects](features.md#several-projects)). The Web UI has one per browser tab.
The SwiftUI app, the Web UI and the Agent are clients of the project core through the same FlatBuffers RPC contract.

`tsugumi-agentd` is the host-side core, one service per host, shared by the Mac app and Web host before any project is open. Alongside Agent conversations, it owns project creation and duplication, bundled demo catalogs and editable copies, and the recent-projects registry (the newest 30 entries). The host-level section of `protocol/agent.fbs` keeps these requests separate from conversation logic; their implementation is in `core/src/host/`. New project documents, the build-time Card Nixpkgs pin, folder names and flow validation are defined there. Both clients only forward requests and translate stable error codes. The registry has one writer and stores Unix epoch milliseconds in `agent-core/recent-projects.json`; no legacy client format is read.

```text
Shared VM (one per app, Virtualization.framework)                 macOS
 tsugumi-vmd (vsock: control 1024 / RPC 1025)
 nix-daemon ── shared /nix/store (store disk)
 project A: tsugumid (own uid, mount/PID namespaces, cgroup) ── vsock ─ 127.0.0.1 relay ── SwiftUI UI (+ isolated WebGPU view modules)
   ├ document, journal, receipts (/project = virtiofs)                         tsugumi-agentd (models, conversations, tools)
   ├ SQLite history, logs, artifacts                                   ── same RPC ──┘
   │                                                                         └ agent.fbs over Unix socket ── SwiftUI / Web host
   ├ scheduler, Nix evaluator (libnixexpr embedded in the process)
   └ worker pool (resident across runs and standby workers, isolated with bubblewrap)
 project B: tsugumid (likewise; stopping A does not affect B)
```

## Directories

```text
CMakeLists.txt, flake.nix/flake.lock  C++ build (CMake/Ninja) and pinned dependencies (Nix)
protocol/          tsugumi.fbs (project RPC) / agent.fbs (host-side core and Agent) / worker.fbs / data.fbs / tsugumi_enums.fbs
core/              tsugumid (project core) and tsugumi-agentd (host-side core);
                   src/host/ (project lifecycle), src/agent/ (conversations and tools)
sdk/               Card ABI, common runtime (libtsugumi_card), C++ codegen (card.hh), Python, R, Julia, Ruby, Go, Rust and C# SDKs and embedded shims, Nix SDK, tsugumi-worker
macOS/             SwiftPM: Sources/App (SwiftUI), Sources/RPC, Sources/Launcher,
                   Container/ (image definitions, builds, versions), Tests/
ui/modules/        Shared isolated web-module host (browser iframe and WKWebView)
agent/             Shared Agent prompts, contracts and reference material
macOS/Sources/AgentClient/  Native adapter to the host-side core RPC
web/               Browser UI and host server (Agent RPC and platform adapters; shared material in agent/Shared)
cli/               The tsugumi command: installers (install.sh, install.ps1), the Mac launch script, scripts that make the distributions
plugins/tsugumi/   The Claude Code plugin (installed from .claude-plugin/marketplace.json)
nix/               core.nix: package definitions of tsugumid, tsugumi-agentd, tsugumi-worker and the Card SDK
                   flake/: the flake's outputs as flake-parts modules (packages, checks, dev shell, docs apps)
tests/core, tests/python, tests/fixtures   Contracts, integration and packaging tests, model fixtures
demos/ja, demos/en The demos, a set per UI language (the same demos and files, their text in one language): the UI's demo list (index.json);
                   basic, science and Plots (matplotlib, Plotly, Altair) demos, and Draft versions of the science demos
docs/              Documents (listed in docs/README.md). docs/assets/ has the icon, key visuals and screenshots;
                   docs/site/ has the Astro site and the moving figures (output: build/docs/, not tracked)
```

Generated bindings (C++/Python/Swift) and BFBS are made by `flatc` into each build's output and are not in Git.

## Main design decisions

- **JSON goes through typed schemas** (hosq). A document is validated whole with `json::GenericDom` (UTF-8, duplicate keys, non-finite numbers) before being decoded into structs, and unknown keys are refused. hosq cannot encode with per-member omission, so **the canonical form of a document file is the validated text received from the client itself**. The core never re-encodes documents, so the original notation of integers (64-bit included) is kept and passed on as it is. JSON internal to the core (journal, view, job and so on) is encoded as structs with all fields required.
- **Saving**: a change is applied provisionally to the document model and validated → the journal (with the contents of all files and the receipt) is fsynced → files are replaced atomically → the commit is recorded. A crash in the middle is reapplied from the journal at startup, so the result is always all applied or none (with fault-injection tests).
- **Saving during a run**: a save that changes the computation's definition (its execution fingerprint) is refused with `busy` while running; layout changes and the like are accepted (so as not to stop drags).
- **pkgsPath**: `tsugumi:default` (the bundled pin) or `https://github.com/NixOS/nixpkgs/archive/<rev>.tar.gz#sha256-…`. Cards' Nix evaluation uses restricted eval, forbids IFD and limits allowed-uris.
- **Ports of built-in Cards**: trigger `out` / input (builds each message of `_outputs.fbs` from params of the same names; for File, copies the chosen project file into the run) / fork `in` → params `branches` / branch `value`, `condition` (value:bool) → `a`, `b` / merge declared inputs → `out` (a union named by input port, `tsugumi.merge.Merged`) / subgraph `boundary`.
- **Isolation**: the VM is given only the open projects, their control folders and the specified Mac folders; inside the VM each project has its own uid, namespaces and cgroup, and Cards run under bubblewrap (Seatbelt for a tsugumid on the Mac). Each route is described in §2–§4 of [Security and isolation](security.md).
- **Where persistent data and the runtime live**
  - **The data's location**: `~/Library/Application Support/Tsugumi/` (another folder for the development version; [development and release versions](#releases). `TSUGUMI_SUPPORT_DIR` can move just the VM and the store elsewhere).
  - **What `vm/` holds**: where the VM's boot disk is unpacked (`image/`, an APFS clone of the one bundled with the app), Tsugumi's source given to the VM read-only (`source/`, what `macOS/Container/vm-source.txt` lists, copied at each boot), the sparse disk image of the Nix store shared by all projects, `store.img` (up to 64 GiB by default, only what is used), and console logs (`console.log`, the previous `console.prev.log`, and the app's own record `launcher.log`). Each project's control folder is in `environments/<id>/control/`.
  - **The store**: the VM mounts the store disk as `/persist`, and `/nix` is an overlay of the rootfs's store (lower) and `/persist` (upper); all projects use one nix-daemon.
  - **Building Tsugumi itself**: tsugumid, tsugumi-vmd, the terminal's tools and the formatters are built by the VM's `/init` with Nix inside the VM from the shared source's flake (`packages.<system>.vm-system`), and started. Dependencies come from cache.nixos.org, and tsugumid is compiled from source. The result is recorded per hash of the source's content in `/persist/system/<hash>` and made a GC root, so it is rebuilt only the first time and when the source changes (an app update, or changes to the core during development). Nothing is downloaded on the Mac side.
  - **Updating the boot disk**: when it has changed (an app update), the store is checked once (`nix-store --verify --repair`). Paths of the previous boot disk that are now gone are fetched again from cache.nixos.org if something (a Card's build, say) refers to them, and removed from the database if nothing does.
  - **GC roots**: each project's build results are `/nix/var/nix/gcroots/tsugumi/projects/<id>/<Card>--<result>` (the folder is created by vmd as root, and its parent is not writable from projects, so another project cannot claim it first). Each time a result is used, a temporary root (held by the daemon connection) and then a permanent root are taken, so checking validity and using it never race with GC. The last two results per Card are kept; roots unused for 30 days, and the root folders of projects not started for 30 days, are collected.
  - **Cleaning and resetting**: "Delete the build cache" in the project's settings only removes that project's roots; cleaning the shared store (deleting what has no root, vmd's `gc`) happens only with "Clean shared store" in the app's settings. With `keep-outputs = true`, the compilers and nixpkgs sources used to build rooted Cards survive cleaning. "Reset all environments" in the settings stops the VM and deletes `vm/` and `environments/`, without touching the run-history disk `vm/history.img`, the projects' documents and artifacts, or the host's `/nix/store`.
- **Where the run history lives**
  - **Primary copy**: the history's SQLite (runs, invocations, states, values, logs, displays, receipts) is `<id>/history.sqlite` on the VM's run-history disk (`vm/history.img`, sparse, up to 32 GiB, mounted at `/history-disk`), visible only to that project's tsugumid as `/history` (`--history-dir`; without it, `.tsugumi/history.sqlite`). Waiting for each commit between Cards no longer goes through a shared folder.
  - **The disk's settings**: kept apart from the store disk (`synchronizationMode: .full`, F_FULLFSYNC on the Mac at every commit) and attached with `.fsync` (records already committed survive the app or the VM crashing; on a power loss of the Mac, the last writes may be lost). It can be reset separately from the store, and survives "Reset all environments".
  - **The copy in the project**: with "Save run history in the project" in the Workflow's settings on (`saveRunHistory` in `tsugumi.json`, default true), finished runs are also appended to `.tsugumi/runs.sqlite` (when writing has paused for 2 seconds, at the latest after 30, and on exit; not counted in the run's time).
  - **Incompatible formats**: the daemon's local history schema is version 4 and records batches and saved input groups. If the local database or the portable copy is incompatible, startup reports the difference and keeps the files in place. In Mac and Web, **Archive History and Start** moves each incompatible database and its SQLite sidecars to a unique `history-archive-<UUID>` directory beside the database. The local archive lives on the VM's history disk on macOS or in the project state directory when running the daemon directly. The portable archive lives in the project's `.tsugumi` directory. A direct daemon launch can opt in once with `--archive-incompatible-history`.
  - **The copy's format**: a fixed shape (`format = tsugumi-runs`, `version = 4` in `meta`). Values are kept as FlatBuffers bytes with their SHA-256, pointing at `schemas` (hash → bfbs and root type), so a reader can interpret them on the spot even if types changed between runs. Logs keep the fragments as written and their positions (preserving the order of the whole Run's log), and artifacts point at files in the project, `.tsugumi/runs/<run>/`. Running state, receipts and last inputs are not included.
  - **Carrying it along**: on opening, runs missing from the VM are imported, matched by `run_id`, so the history follows a project that was copied or moved to another Mac.
  - **Deleting**: "Delete Run History" (`RunHistoryDelete`; everything, or what finished before a given time; not while running) deletes from three places: the VM, `.tsugumi/runs.sqlite` and the per-run files. The history in the VM is not deleted automatically; under "Run history in the VM" in the app's settings you can see its size and also delete that of projects whose folders are gone (vmd's `histories` and `forget`).
- **Preventing double starts**: the VM is started only by the one app holding the flock on `vm/vm.lock` (a second app stops with "Another Tsugumi is using the runtime"). The core takes a non-blocking flock on `.tsugumi/tsugumid.lock`, and before opening the document also checks that a second flock on the same file is refused (that the location really enforces locks).
- **A Card's stages**: waiting for inputs → SDK preparation (generating the contract of inputs, outputs and params) → Nix evaluation (the evaluator embedded in tsugumid, after resolving the worker kind and target system) → realisation (nix-daemon in the shared store for native workers; the remote worker in its own store for remote workers; nothing if the result is present) → run (`preRun → run → postRun`) → output validation → saving → done. For each invocation the core records the stages passed, with start and end times and results (done, cache used, reused as prepared in this Run, failed, stopped), in `InvocationSummary.stages`, and returns them in both notifications and the history. It also keeps whether the worker was reused (`worker_reused`), the output ports that actually sent values (`outputs_sent`; for a branch, only the chosen side) and when the inputs were complete (`ready_at`). The UI builds the stage display and the highlighting of wires from this record alone, never guessing from the final state. There is one deadline for the whole invocation, and cancellation and the deadline are checked before each stage starts. preRun / postRun run per invocation in its working directory even when the worker is reused, and receive none of the Card's inputs (preRun prepares; postRun handles artifacts and cleanup: `TSUGUMI_OUTPUT_DIR`, the output ports sent `TSUGUMI_OUTPUTS`, `TSUGUMI_FAILED`). preRun's environment variables and `cd` carry over to later stages, and postRun runs as cleanup even after a failure (`TSUGUMI_FAILED=1`; the earlier failure is not erased). After stopping or a deadline, nothing starts, postRun included.
- **Derivations and execution targets**: the scheduler calls the execution service's `prepare` before evaluating. `ExecutionTarget` fixes native versus remote worker and the system; native defaults to the host system, while remote defaults to the fresh daemon probe's `native_system`. Host-side `NixEvaluator::evaluate` produces `WorkerDerivation` (`core/src/worker_derivation.hh`: `.drv`, expected output and system) without building, and its nixpkgs and flake caches include that system. `NixRunner` makes the input closure available through `DerivationClosureTransport`. For native workers it then calls `realise_native` through nix-daemon and passes the output paths directly to the worker. Remote workers receive the derivation and invocation and realise in their own store; they never evaluate. Derivation sharing is mandatory for every remote assignment, and its transport is separate from the invocation's run data. The planned cache reuses derivations and built outputs only, never invocation results; retention of run data remains governed by `tsugumid`. Archive export/import lives only in `derivation_transport.cc`, behind that interface. Unknown remote systems and evaluation errors fail assignment before upload. See [Remote execution](RemoteExecution.md) for the current transport and planned shared cache.
- **The Card SDK**: `default.nix` makes the SDK from the contract, and the Card's body is written in `run`. If it is a derivation (package), the worker loads its `lib/main.so` and calls it in memory; if it is a string, it runs as bash. `run` can also be an ordered list of these implementations. The SDK generates and builds only the languages listed in `languages`, and the parts that do not depend on the contract (visualisation, context, logs and so on) are the common `libtsugumi_card.so`, built once per environment (what is made per Card is just the types and a thin adapter). inputs / outputs / params can also be passed explicitly to `sdk { languages = [ … ]; }`. Examples of `default.nix` and of each language's code are in [Your first Card](CardGuide.md) and [Features §3 and §4](features.md#3-reproducible-environments-with-nix).

  `Card::Input` has a member per input port (`std::optional` for optional inputs, a union named by input port for a merge), `Card::Output` has a `std::optional` per output port (only those set are sent), and `Card::params()` gives the params (numbers and bools are `flatbuffers::Optional`, which can represent unset, so read them with `.value()` / `.value_or(x)`). File / Directory are paths in `.value`, and the output location is `tsugumi::context().output_directory`. Fail with `throw tsugumi::Failure(kind, message)`, and check for stopping with `tsugumi::cancelled()`. Python has `main(ctx)` and an optional `on_run_start(ctx)`. A Card need not include generated headers (`_inputs_generated.h` and the like).
- **Dependency locks**: a Card's lock belongs to the Card (a file in its folder), and external packages are used only when the Card's `default.nix` names the lock and the hash of what it fetches (`pythonCard { lock = ./uv.lock; hash = "sha256-..."; }`). Nothing locks automatically: making or updating a lock is an explicit act of a person or the Agent, so a Card never changes its dependencies on its own between two runs. For Python, a fixed-output derivation runs the pinned `uv export --locked` and `pip download --require-hashes --only-binary :all:` for the Card's interpreter, and a second derivation installs those wheels offline (`autoPatchelfHook` on Linux) into a directory that precedes the nixpkgs environment on the embedded interpreter's path. The fixed-output derivation's name carries the hash of `uv.lock` and `pyproject.toml`, so an edited lock with the old `hash` is a hash mismatch, not the previous wheels. Wheels only: a source distribution built inside a fixed-output derivation would not have stable bytes. This is done in the SDK rather than with a flake input such as uv2nix, which reads the lock while evaluating and would add an input to every project's pinned sources; the evaluator rejects import-from-derivation, and here the lock is only hashed at evaluation time.
- **Native workers** (`tsugumi-worker`): the project's worker pool holds a worker per Card. By default (`reuseWorker` false) every invocation gets a fresh worker, so a Card keeps no state; the next fresh worker is started ahead each time one is used (the sandbox, starting the worker, loading `lib/main.so` and Python imports; preRun is per invocation). A worker with `reuseWorker = true` stays resident across invocations and runs (static state included; `onRunStart` / `on_run_start` at the start of each run), and ends when build or run conditions change, on failure, cancellation or a deadline, when the project stops, after 10 minutes unused, above 2 GiB of memory, or as the oldest once more than 32 are resident. Loading happens in the worker's own `work/`. The body's environment variables are, from weakest to strongest: the values when the worker started, the invocation's values, values the Card set while loading (at the top of `main.py`; they last while resident), and preRun's exports; Python's `os.environ` is aligned to this per invocation too. Loading comes before preRun, so settings libraries read when loading (thread counts for OpenBLAS or libgomp, say) must be set at the top of `main.py`, before the imports. The SDK does not import `flatbuffers`, which loads numpy, first, so this is in time. Workers are started on a long-lived thread of the pool (bubblewrap's `--die-with-parent` stops the worker when the thread that started it ends). For `filesystem: false` Cards, `work` and `tmp`, and the files of preRun / bash's run / postRun, are in a per-worker temporary area inside the VM (not the project's shared folder), emptied between invocations. `out` is made only for Cards with File / Directory outputs, and kept for downstream and artifacts. From inside the sandbox, what is visible is the project's `runs` and saved inputs (read-only, bound once at start) and the worker's own temporary area; the invocation's output folder is passed as an FD with `SCM_RIGHTS` and shown as `/proc/self/fd/900`, and the core maps it back to its real path and validates it. The working folder is not passed as an FD (`getcwd` fails for a current folder pointing outside the sandbox). `filesystem: true` Cards also run in the temporary area, and `work` and `tmp` are copied into the run's folder after the invocation. Inputs are not passed as FDs because reopening through `/proc/self/fd/N` can break read-only (inputs are protected with read-only mounts). The Mac development environment (without bubblewrap) uses the real paths as they are.
- **bash inputs**: for each port `P`, `JSON_P` (the path of a JSON file, with no size limit; `jq` is available), and for File/Directory, `FILE_P`/`DIR_P`. The input JSON and its converter are made only when running a bash `run`. The output is the JSON in `$TSUGUMI_OUTPUT`, type-checked per port (separate from the artifact's `out`, so it works for Cards without artifacts).
- **Nix exports across a connection**: a Code Card reads the `exports` of a connected Code Card through `inputs.<destination-port>`. The resolver supplies the exports of one upstream producer that always runs first. It reports unknown, ambiguous or unsafe references before Nix evaluation where possible. `inputs.tsugumi` and `inputs.sources` retain the SDK and the project's pinned sources. The evaluator passes Card sources (except `_card.json` and `_meta.json`), input types, Nix parameters and connected exports. Run-time-only parameters stay with the invocation. Build outputs in the Nix store are reused by derivation output path. One evaluator per tsugumid reuses nixpkgs, the evaluation state and the daemon connection across Cards and runs. Cancellation and `buildTimeoutSeconds` interrupt the evaluator's thread.
- **Card in Card**: a child Workflow maps outer ports to inner Cards' ports with `boundary.inputs/outputs`, and from outside it is connected and run like an ordinary Card (it can be the target of a Run; run alone, it uses saved values at its boundary inputs). State and history are told apart by full Card path. It is a different thing from visual-only groups (layout.json).
- **Summaries**: a Card's summary is kept only in `description` of `_meta.json`, whatever its kind (Cards in Cards too; not in `_card.json`). `_meta.json` is not part of Nix evaluation, so rewriting it causes no rebuild.
- **Drafts**: `_meta.json` holds the summary, the processing, constraints, the conditions for completion and the purposes of inputs and outputs. Ports are made automatically when wires connect (inputs in `_card.json`, outputs in `_meta.json`; removed when the last wire is cut) and can be wired without types. A Draft never runs or counts as succeeded; Cards depending on a Draft are blocked with a reason, and independent parts run as usual. "Generate Cards" gives the Agent the Drafts, wires and descriptions, and has it save types, Nix and sources in one PutDefinition/PutSource transaction before validating and running (on failure or cancellation in the middle, nothing is saved and the plan stays). When an Agent's save (`origin` `agent`) turns a Draft into something other than a Draft, `Service::save` inspects `_meta.json` (`permissions::incomplete_draft_intents`, inside Cards in Cards too) and saves nothing, with `validation_failed`, if `description` is empty, if the Draft had a `description` or `content` but `context` is empty, or if the Draft's `description`, `content` or `context` changed. What the text says (adding nothing that was not written) is a promise of the prompt; what can be checked is only whether it is empty and whether it changed. A save that deletes the Draft and replaces it with a Card of another id is not covered, nor are people's saves.
- **The Agent**: the host-side `tsugumi-agentd` service in `core/src/agent/`, with prompts and reference material in `agent/`. Tool definitions are generated from `tsugumi.bfbs` (with the FBS doc comments), and tool arguments ⇄ FlatBuffers are converted with the same BFBS (64-bit as decimal strings). The Agent gives each tool call an op_id and persists it on the host side with the conversation and the tool trail. A change whose response was lost is confirmed with OperationGet, never blindly resent. PNGs and JPEGs in tool results (image displays in `run_record`, reading files and artifacts) are sent to the model as images, not as strings (`core/src/agent/tool_images.cc`). Partially read or oversized images are not sent, with a note instead, and if the provider does not accept images, the conversation is resent with its images removed. What is needed for Python, C++ and bash Cards is in the instructions, so the model starts writing without reading references. The list of the Python SDK's methods is made from the SDK's source by `agent/Shared/generate.py` and put into the instructions. `demo_list` and `demo_read` expose bundled read-only demo references; project files are read through the project RPC tools. Platform adapters read the model's API keys from the Keychain (or the Web host's environment or credential file) and pass them to `tsugumi-agentd`, which persists provider configuration in its host-side `agent.sqlite`. They are never given to the project's `tsugumid` or workers. Cancellation stops the model's communication and unsent tools, but not runs already accepted.

## Releases

- **The version number** is only in `VERSION` (read by the core, Nix, CMake and the app). The stable release starts at v1.0.0, and compatibility is not kept during the 0.x versions before it ([AGENTS.md](../AGENTS.md)).
- **Making a release**: in a checkout of origin/main, `macOS/tools/release.sh 0.2.0` rewrites `VERSION`, commits it and pushes it to main with the tag `v0.2.0`. Without a checkout, `gh workflow run release.yml -f version=0.2.0` (Actions does the same). Either way the release workflow (`.github/workflows/release.yml`) builds the app and creates a GitHub Release, which installed releases find and update to automatically (`macOS/Sources/App/Model/Updater.swift`).
- **Making a release candidate**. Run `macOS/tools/release.sh 0.2.0-rc.1` from a checkout of origin/main. `VERSION` and the packaged app and command report `0.2.0`. The workflow publishes the suffixed tag as a GitHub prerelease. Download the app from the candidate's release page. Set `TSUGUMI_VERSION` to `0.2.0-rc.1` when running a CLI installer for the candidate. The app's automatic updater follows stable releases. The app needs a manual reinstall to switch to the stable release of the same numeric version. The `tsugumi update` command uses the latest stable release.
- **The `tsugumi` command**: the same workflow puts `cli/install.sh`, `cli/install.ps1`, and the `tsugumi-macos-arm64.tar.gz` and `tsugumi-linux.tar.gz` made by `cli/package-macos.sh` and `cli/package-linux.sh` (each with a `.sha256`) on the release. The scripts are attachments of the release rather than files on the documentation site: the site updates on every push to main, so it would show scripts for versions with no distributions yet, whereas as attachments the scripts and the distributions are always the same version and arrive as they are through `releases/latest/download/`. The Node.js version and checksums are in `cli/node-version.env`. To try it locally, pass a folder holding the distributions as `TSUGUMI_INSTALL_BASE=file://…`, and keep the VM apart with `TSUGUMI_SUPPORT_DIR`.
- **The VM's boot disk** (`vm-boot.nix` and the pinned nixpkgs): after changing it, publish it to [gen740/tsugumi-vm](https://github.com/gen740/tsugumi-vm) before releasing, with `macOS/Container/build-image.sh --vm` and `macOS/tools/publish-vm.sh` (which also publishes the list of licenses and the corresponding source with the disk). The runners cannot use Apple container, so the release build fails without a published disk.
- **Development and release versions** (`AppChannel.swift`): `build-app.sh` makes the development version by default (`Tsugumi Dev`, bundle id `app.tsugumi.Tsugumi.dev`, `TsugumiChannel` `dev` in `Info.plist`), and with `--release` the release (`Tsugumi`, `app.tsugumi.Tsugumi`, `release`). The support folder (VM, store, control folders, conversation history) is `~/Library/Application Support/Tsugumi-dev/` for the development version and `Tsugumi/` for the release, and user settings (UserDefaults) are separate per bundle id too. Checks and tests during development (UI scripts, smoke, `swift test`) use the development version, so they never touch the VM of the release you use. The Python tests' shared VM is `build/test-vm`.
- **Signing development builds**: by default both are signed ad hoc. With ad hoc signing, the designated requirement is the hash of that build's binary, so the login keychain treats every rebuild as a different app and shows a permission dialog (reading the API key, for example). The development version can be signed with a certificate in this Mac's keychain (Apple Development or the like) named in the environment variable `TSUGUMI_CODESIGN_IDENTITY`: `TSUGUMI_CODESIGN_IDENTITY="Apple Development" macOS/build-app.sh` (part of the certificate's name, or its SHA-1, as `codesign --sign` takes; list usable certificates with `security find-identity -v -p codesigning`). The designated requirement becomes the certificate and bundle id, unchanged across rebuilds. `--release` ignores the variable and stays ad hoc (release artifacts do not depend on the environment). The first time after switching from ad hoc, the keychain dialog appears once (choose "Always Allow", or save the key again); after that it does not. The `com.apple.security.virtualization` entitlement the VM needs is attached with either signature.
- **Automatic updates** happen only in releases. 20 seconds after launch and every six hours, the app reads `api.github.com/repos/gen740/Tsugumi/releases/latest`, and if a version newer than the current one (not a draft or pre-release) has `Tsugumi.zip` and `Tsugumi.zip.sha256`, downloads them into a temporary folder on the same volume as the app and checks the checksum, `codesign --verify --deep --strict`, the bundle id, `TsugumiChannel` and the version number. The replacement happens in `applicationWillTerminate` (after stopping the VM) with `FileManager.replaceItemAt`; for "Restart to Update", the new app is opened after waiting for this process to end. No checks happen while a UI script or smoke run is going.

## The documentation site

The Markdown files in `docs/` are the source documents. The repository's `README.md` is the entry point; `THIRD_PARTY_NOTICES.md` stays with the license. Edit these sources, never a generated copy. [docs/README.md](README.md) supplies the navigation groups and descriptions.

The site is an Astro project in `docs/site/`, with its own pinned dependencies. Nix supplies Node.js and builds the site's packages from `docs/site/package-lock.json` ([nix/docs-site.nix](../nix/docs-site.nix)); `docs/site/node_modules` is a link to them, and nothing is installed with npm.

```bash
nix run .#dev-doc
```

Astro serves the site at `http://localhost:4321` and reloads when documents, components or illustrations change. To build and check the static output:

```bash
nix run .#build-doc
nix run .#test-doc
```

- **Output:** `build/docs/` contains the HTML, linked Markdown downloads, `llms.txt` and static assets. It is ignored by Git and can be deleted and rebuilt. After one of the commands above has made the link, `npm --prefix docs/site run preview` in the dev shell serves that output locally.
- **Content:** `src/content.config.ts` loads the source documents into an Astro content collection. Astro renders Markdown; the `.astro` components provide the common layout, navigation and home page. `src/lib/documents.mjs` defines the story illustrations and resolves repository links for the website and Markdown downloads.
- **Publication:** [.github/workflows/docs.yml](../.github/workflows/docs.yml) builds and checks the site on pull requests and publishes `build/docs/` on pushes to `main`. Set the repository's Pages source to **GitHub Actions**. No generated pages are committed.
- **App help:** `macOS/build-app.sh` builds the same site and `macOS/tools/bundle-docs.sh` bundles its story pages and assets. The development app reads `build/docs/` when no bundled help is present. Relative URLs let the pages work under both GitHub Pages and the app's local URL scheme.
- **Illustrations:** the Canvas modules in `docs/site/js/` run only on pages that use them. Story figure positions are defined in `STORY` in `src/lib/documents.mjs`; a missing anchor phrase fails the build.
- **Demo player:** `docs/site/player/` builds the interactive gallery from the Web UI. Its shared outputs (`docs/site/js/player.js`, `docs/site/player.css`, `docs/site/player-data/`) are also used by the Web UI and native thumbnails. Regenerate them with `node docs/site/player/build.mjs` after changing the player inputs; `node docs/site/player/build.mjs --check` verifies them. This needs `web/node_modules` and `web/src/gen`.
- **Without JavaScript:** document text and links remain readable. Animation respects reduced motion.

## Tests

Run the platform checks with `tests/ci-local.sh`. On macOS this runs the Mac checks locally and the Linux checks in Docker or OrbStack. Use `tests/ci-local.sh mac` or `tests/ci-local.sh linux` for one platform, and `tests/ci-local.sh agent` for the public Agent RPC fixtures.

The CI definitions are [ci.yml](../.github/workflows/ci.yml) and [ci-platform.yml](../.github/workflows/ci-platform.yml).

| Location | Coverage |
| --- | --- |
| [tests/core/](../tests/core/README.md) | Core behavior, public RPC and integration tests |
| `tests/python/` | Packaging, release, licenses and host integration |
| `macOS/CoreTests/`, `macOS/Tests/` | Agent, RPC and app behavior |
| `web/` | Browser UI type checks and behavior contracts |
| `tests/prompt/` | Offline prompt probe scoring |

For documentation changes, run `nix run .#build-doc` and `nix run .#test-doc`. Changes to the demo player also require `node docs/site/player/build.mjs --check`.

### R and Julia worker measurement

`tests/python/measure_language_workers.py` runs each R/Julia branch of the Collatz demo alone, starting at 27 (112 values, 335 language Card invocations). The demo's Cards set `reuseWorker: true`, so the "Resident" columns measure a kept interpreter. A Card without that setting starts a new interpreter per invocation and is not what these numbers describe; that default path has not been measured. Run it with `TSUGUMI_SDK_DIR` and `TSUGUMI_WORKER` pointing at the old and new SDK/worker, keeping the daemon and nixpkgs fixed. It checks the final sequence length and prints each run's wall time, stage totals and worker reuse count.

On an Apple silicon macOS development host, with sandboxing disabled, the second run in the same daemon measured:

| Language | Before: wall | Resident: wall | Before: execute total | Resident: execute total |
| --- | ---: | ---: | ---: | ---: |
| R 4.6.1 | 102.284 s | 0.357 s | 161.112 s | 0.270 s |
| Julia 1.13.2 | 119.070 s | 0.493 s | 186.880 s | 0.364 s |

This comparison used the SDK at `03837b42` as the baseline and nixpkgs `1c89083e451e891152365b799595cf7790a43325`. The sum of execute stages includes concurrent Cards, so it can exceed wall time. These are two runs per configuration on one development machine, not a cross-platform benchmark. The resident first runs took 17.125 s (R) and 10.667 s (Julia), including new shim builds; their execute totals were 3.186 s and 5.273 s. The baseline first runs used already-built packages and took 97.803 s and 123.141 s. No Python/C++ demo Cards or UI rendering are included in this measurement.

## Go Card implementation

Go uses `-buildmode=c-shared` and the existing C ABI. A subprocess/JSON adapter
would add serialization and a separate child lifetime/display protocol; the
library path reuses the worker's typed validation, sandbox, cancellation,
deadline and resident-state behavior. A worker loads one package and never
calls `dlclose`, so the Go runtime lives until that worker process exits.

The C shim uses the current `abi.h`. `Context` holds a shared guarded state,
not a copyable raw pointer: every callback locks the guard, and closing the
invocation clears it before C returns. The SDK copies C input buffers to Go
memory and unpacks them; it does not expose C-backed slices. Output bytes and
numeric arrays cross cgo only for a synchronous callback that copies them.
String pointer arrays are allocated in C, with C-owned strings. These rules
follow [cgo's pointer ownership contract](https://pkg.go.dev/cmd/cgo#hdr-Passing_pointers).

`ctx.Go` tracks tasks, recovers their panics and waits before output packing.
Normal call completion closes its context and joins its cancellation monitor.
Error/cancellation paths close the context before returning; the failed worker
is discarded. Cancellation/deadlines kill all its goroutines. Untracked user
`go` statements are the Card author's responsibility and a panic in one can
crash the worker, which the daemon reports as a failure. Shell hooks may run
with the Go runtime already loaded. The child does only process setup and
execs Bash, while the original worker owns the Go threads and state.

The generated SDK contains FlatBuffers' Go runtime from nixpkgs' fixed
FlatBuffers source. Other modules use the Card's explicit `lock` (go.sum)
and `hash` for buildGoModule's fixed-output proxy cache. The actual Card build has network
resolution disabled and uses `-mod=readonly`. There is one root package,
not a selectable Go workspace/package list. The source of both UI scaffolds
is `sdk/go/template`; run `python3 sdk/codegen/scaffold_go.py` to synchronize
Swift and TypeScript after changing it.

`tests/python/test_go_cards.py` runs real host sandboxes and covers typed
Python/Go/C++ data, stale context copies, retained owned inputs, managed task
outputs (also with `GOEXPERIMENT=cgocheck2`), lifecycle, panic/error recovery,
cancellation/deadlines, files and every display kind. The bilingual
`go-pipeline` example and its Draft are bundled demos.

## Module display performance checks

`web/tools/module-perf.tsx` uses the production display component with three deterministic cases: eight Plotly traces of 5,000 points, a Vega-Lite scatter plot of 50,000 points, and twelve mixed Plotly/Vega-Lite figures of 2,000 points each. It records initial output readiness, 120 animation-frame intervals while changing the display size and position as in canvas pan/zoom, retained iframe count, and `performance.memory.usedJSHeapSize` when available.

Build the comparison fixtures with:

```sh
node web/tools/module-perf.mjs /tmp/modules-before 03837b42
node web/tools/module-perf.mjs /tmp/modules-after
python3 -m http.server 8771 --bind 127.0.0.1 --directory /tmp/modules-before
# In another terminal:
python3 -m http.server 8772 --bind 127.0.0.1 --directory /tmp/modules-after
```

The baseline fixture adds the same iframe sandbox that the production server applies through CSP. Open both pages in a background browser with at least a 1280 × 720 viewport. For each case choose the scene, Render, wait for the results line, then Measure pan and zoom. Close all between cases. New result changes the data without changing its length; Open expanded uses the full interactive viewer.

The Web canvas now retains images instead of one plotting runtime per figure. Visible frames share one serial renderer with a 32 MiB preview-cache budget (encoded data plus decoded-pixel cost); it is removed one second after the queue empties. Hidden frames release their displayed image and cancel queued work. The full JSON and dimensions identify a preview, and small rounding changes during zoom reuse its dimensions. Initial PNG generation has a cost, especially for many figures; the optimization removes retained runtimes, rather than promising a shorter first render in every case.

A manual comparison on 2026-11-12 used the same background Chromium browser at 1280 × 720. The baseline was `03837b42`, with the production sandbox applied. Each row is one run, not a statistical estimate; other work on the shared Mac can affect timings.

| Scene | Initial output, before → after | Pan/zoom frame interval p50 / p95 / max, before → after | Retained iframes, before → after |
| --- | --- | --- | --- |
| Complex Plotly (2,251,466 source bytes) | 355.5 → 402.7 ms | 16.7 / 18.3 / 18.7 → 16.7 / 18.3 / 18.5 ms | 1 → 0 |
| Large Vega-Lite (3,991,328 source bytes) | 253.5 → 352.9 ms | 16.7 / 18.3 / 18.7 → 16.7 / 18.3 / 18.6 ms | 1 → 0 |
| Twelve mixed figures (2,332,516 source bytes) | 505.9 → 605.7 ms | 16.7 / 18.3 / 150.0 → 16.7 / 18.3 / 33.4 ms | 12 → 0 |

For completeness, `usedJSHeapSize` readings in bytes were 20,152,105 → 23,707,679 (complex), 22,134,281 → 27,472,890 (large), and 22,050,521 → 22,556,428 (twelve figures). These are consecutive same-tab readings, not controlled memory profiles.

The twelve-figure case displayed all twelve previews; another result with the same-sized data displayed all twelve updated previews in 603.6 ms. Its expanded view showed the updated revision, zoom changed the axis ticks, and closing it removed the interactive iframe. Initial export was slower in all three cases; steady-state median and p95 frame intervals were essentially unchanged. The lower maximum in the twelve-figure run is an observation from this sample, not a guaranteed latency bound. JavaScript heap readings are not used as evidence of a memory reduction: they may omit isolated iframe heaps and are not whole-process RSS. Retained iframe counts demonstrate the runtime-lifetime change; measuring total browser memory requires a separate process-level profile.

The viewer uses [Plotly image export](https://plotly.com/javascript/plotlyjs-function-reference/#plotlytoimage) and [Vega view finalization](https://vega.github.io/vega/docs/api/view/#view_finalize) at their documented lifecycle boundaries.

The common viewer's lifecycle check also runs in real, offscreen macOS WKWebViews, without opening or activating a window:

```sh
swift macOS/tools/perf/module-viewer.swift /tmp/modules-after/modules
```

This creates and closes three viewers, verifies rapid same-sized Plotly updates show the latest values, exports PNG, replaces Vega views five times, and switches back to Plotly. On 2026-11-12 it passed in 1.2–2.0 seconds across repeated checks. Against the baseline viewer it failed because none of the five replaced Vega views were finalized. The Node behavior suite separately tests queued-update coalescing, stale completion suppression, zero initial iframe viewport dimensions, cache eviction, cancellation, and idle cleanup. These checks complement the existing macOS canvas measurements in commit `03837b42`; this change does not replace that native thumbnail implementation.

## Rust Card implementation

Rust uses a `cdylib` loaded by the existing resident worker. A subprocess/JSON
adapter would simplify the ABI boundary but add serialization and process setup,
lose FlatBuffers borrow checking and require a second state/display protocol.
The library adapter reuses cancellation, deadlines, sandboxing and streamed
Display callbacks. Its C shim (`sdk/src/rust_host.c`) compiles against the current
ABI header; Rust receives an opaque call pointer instead of duplicating that
layout. There is no dependency on the R/Julia JSON adapter.

`sdk/codegen/card_rust.py` generates verified borrowed `Input` views, owned
`Output` objects, params and the exported entry points. The safe context ties
views to the call and is neither `Send` nor `Sync`; only the runtime touches raw
pointers. `Result` failures and unwind panics become structured Card failures.
The release profile uses unwinding; the panic payload is forgotten so that a
custom payload's destructor cannot panic across C. The failed worker is then
discarded. Native aborts and double panics remain worker crashes, isolated from
the daemon.

`rustCard` uses the FlatBuffers Rust runtime from the same pinned source as
flatc. For a Card without registry dependencies, a lock in the SDK pins its
own supporting crates. For external crates, the Card's `lock` and `hash` pin
a vendored directory built from Cargo.lock. Edits to Rust source reuse that
directory. Cargo still recompiles the dependency code on a Card rebuild.
The SDK builds a single package with a `cdylib`. `buildInputs` supplies
native libraries and `nativeBuildInputs` supplies tools. The lock helper
resolves in a temporary source copy and leaves only Cargo.lock behind.

The UI scaffold's source is `sdk/rust/template/`; after changing it, run
`python3 sdk/codegen/scaffold_rust.py` to update both Swift and TypeScript.
`tests/python/test_rust_cards.py` exercises the real host sandbox (Seatbelt on
macOS, bubblewrap on Linux), typed Python/Rust/C++ data, borrow-check failures,
worker state, panic recovery, cancellation/deadlines, file artifacts and every
Display kind. The bilingual `rust-pipeline` demo is the small end-to-end example.


### Card-owned display modules

`protocol/data.fbs` defines `DisplayModule` as a Nix store directory, a JSON
configuration and named binary blobs. `sdk/nix/view-module.nix` bundles a view's
local dependency graph to the directory's `index.js`; `builtin-views.nix` builds
Plotly/Vega-Lite through the same mechanism. Their pinned upstream package URLs
and hashes live in `sdk/views/packages.json`, with libraries evaluated only
inside the isolated frame. A Card's `views` declarations are recorded as store
references in its manifest's pre-run environment. The worker resolves these
names before publishing a display. Retention is unchanged.

`ModuleReadRequest` serves self-contained source in bounded `FileContent` pages.
For `wasmViewModule`, `sdk/nix/wasm-view.nix` builds a small Card side module and
links `index.js` to one shared Emscripten/Emdawnwebgpu template output.
`core/src/module_source.cc` assembles that entry with `view.wasm` at read time.
The standard C WebGPU header comes from nixpkgs `wgpu-native.src`; the portable
input/event header is `sdk/include/tsugumi/view.h`. The shared host owns canvas,
surface configuration, event translation, animation and snapshots; GPU calls use
Dawn's pinned port rather than a second binding implementation.
The core rejects non-store directories, shared targets other than canonical
store `index.js` entries, and Wasm files escaping the Card's directory. UI code caches by
immutable identity. `ui/modules/viewer.js` drives initialise/update/resize/dispose
inside an opaque iframe. Mac loads only the trusted host HTML into a
nonpersistent WKWebView without granting file access, and denies network/file
requests and non-about navigation. See the [SDK interface and isolation
limits](CardGuide.md#card-owned-web-views). Binary inputs use transferable buffers
between browser frames after making the copy required to preserve retained
displays. WKWebView's argument bridge currently uses base64, so that path copies
and expands bytes; it does not claim zero-copy native transport.

The module viewer performance scripts above describe the previous built-in-only
viewer and are not verification of the new RPC loading path. Current lifecycle,
loading and thumbnail behavior tests are in `web/tests/moduleViewer.ts` and
`figurePreview.ts`; the runnable GPU fixture is `tests/fixtures/view-point-cloud`.
Check host WebGPU support without a window or activation with:

```sh
nix develop -c /usr/bin/swift tests/tools/wkwebgpu.swift
```

The probe uses accessory activation policy, a nonpersistent offscreen WKWebView,
a secure localhost base URL, `navigator.gpu` and `requestAdapter()`, and exits
within 20 seconds. A timeout or denied WebKit/GPU service is not evidence that
WebGPU is unsupported. The built-in line chart now uses the same isolated view
module on both platforms. The C/Wasm alternative uses the same loader; see
[the SDK view guide](CardGuide.md#cc-views-with-standard-webgpu). An optional
RPC-relayed live worker channel remains future work (never a socket from the frame).


The built-in `line-chart` view lives in `sdk/views/line-chart.js`. Its metadata
contains series IDs, labels, offsets and counts. Its `x` and `y` blobs contain
concatenated little-endian float64 coordinates; the worker keeps all language
SDK line-chart calls unchanged and encodes this module display. There is no
line-chart-specific protocol value or UI renderer. `line-geometry.js` subtracts
the visible origin and normalizes in JavaScript double precision before creating
float32 GPU vertices. Tests cover timestamp-sized values with small differences.
The shared host passes `size.colorScheme` (`light` or `dark`), and preview cache
keys include appearance. The module owns its legend, colours and WebGPU resources.
