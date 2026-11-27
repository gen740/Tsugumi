# Glossary

The words used in Tsugumi's documents and screens. See each row's link for details.

## Assembling processing

| Term | Meaning |
| --- | --- |
| **Workflow** | A graph of Cards connected by wires. The same thing a person draws, the Agent edits and the runtime executes ([Rationale](design/Rationale.md)) |
| **Card** | A unit of processing, with input and output ports and settings ([Cards and Workflows](design/Workflow.md)) |
| **Card type** | What a Card is, stated as `"type"` in its `_card.json`: `code`, `draft`, `trigger`, `merge`, `collect` or `subgraph` ([Kinds of Card](design/Workflow.md#kinds-of-card)) |
| **Code Card** | A Card whose `default.nix` builds its processing with a language SDK into `lib/main.so`, usually from sources in `src/` ([Your first Card](CardGuide.md)) |
| **Built-in Card** | A Card kind managed by the core. Trigger starts a run, Input emits built port values, Merge forwards arrivals and Collect aggregates them |
| **Draft** | An unimplemented Card with only a name and content. Wire Drafts up to draw a plan (they have no ports) and have the Agent implement them |
| **Card in Card (child Workflow)** | A Card with a Workflow inside. From outside it looks like one Card; open it to go inside |
| **Definition** | The folder `definitions/<id>/` that is a Card's substance. One per Card |
| **Port** | A Card's input or output. Output types are written in `_outputs.fbs`; input types are derived from the connected outputs |
| **`maxRuns`** | The most times a Card can be triggered in one Run. A trigger can start multiple invocations when it uses a run list or receives a batch |

## Running and recording

| Term | Meaning |
| --- | --- |
| **Run** | One execution of a Workflow. Records the version (commit) at its start |
| **Invocation** | One execution of a Card within a Run. In a loop, the same Card is invoked many times |
| **Batch** | An ordered set of values sent together on one wire. A run list or a batch input can produce several invocations for one trigger. An ordinary FlatBuffers vector is one value, not a batch |
| **Implementation label** | The name of one implementation: a run list entry's `label`. Shown in the history and used to pair members when comparing runs |
| **Stage** | A step within an invocation: schema generation, Nix evaluation, build, run, output validation, saving ([Inspect a stage](RunHistory.md#inspect-an-invocation)) |
| **`skipped`** | A Card whose connected input cannot arrive because its producer omitted the output or skipped |
| **`blocked`** | A Card that cannot run because of a failed, blocked or cancelled upstream Card. Comes with a reason. Drafts and paths requiring their outputs receive no execution state |
| **`interrupted`** | A local or remote run left unfinished when its submitting `tsugumid` ended; recorded as `failed` with failure kind `interrupted`, with no result rescue or automatic rerun |
| **Partial run (single run)** | Running only the chosen Cards, or "from here". Uses saved inputs ([Rerunning from the middle](design/Rationale.md#rerunning-from-the-middle)) |
| **Saved inputs** | The full set of inputs of the latest trigger, including any batches. Partial runs use these inputs independently of `recordIO` |
| **`recordIO`** | A setting that keeps the input and output values of every invocation of a Card in the history |
| **Artifact** | A file or folder a Card output |
| **View** | A display surface a Card declares in its `default.nix`: a name, a type (`html` or `dawn`) and one file ([Views](design/CoreAndSDK.md#views)) |
| **Display** | What a Card sends to one of its views while it works: text, a table, an image, a chart and so on. Live only: displays are not recorded, so check results in outputs and artifacts ([Visualisation](design/CoreAndSDK.md#views)) |
| **Version** | A commit of the project's git. A named version is an annotated tag ([Versions](Versions.md)) |

## The runtime

| Term | Meaning |
| --- | --- |
| **`tsugumi-hostd`** | Host service owning Agent runs, settings, Skills, providers, execution authorization and project environment lifecycles ([Architecture](design/Architecture.md)) |
| **`tsugumid` (the core)** | A process, one per project, that handles the document, the scheduler, Nix evaluation and history |
| **RPC** | The contract by which the UI, the Web and the Agent talk to `tsugumid` ([Protocol.md](design/Protocol.md)) |
| **`op_id`** | The ID of a request that changes something. A resend with the same ID is applied only once |
| **rev** | The document's revision. A save is based on it, and a save based on an old one is refused as a conflict |
| **Shared VM** | The one Linux VM that the Mac app and the Mac's `tsugumi` command share. The signed helper holds `vm.lock` and hostd manages its resident project environments ([Architecture](design/Architecture.md)) |
| **`tsugumi-vmd`** | The managing process inside the VM. Starts each project's `tsugumid` with its own uid, namespaces and cgroup |
| **`tsugumi-workerd`** | The persistent listener on a remote worker machine; starts `tsugumi-worker` processes to run Cards ([Remote execution](RemoteExecution.md)) |
| **_language_ SDK** | The swappable part for one language (C++ SDK, Python SDK, …) that turns a Card's sources into `lib/main.so`; chosen per Card in `default.nix`. The core knows no language ([The core and the language SDKs](design/CoreAndSDK.md#terms)) |
| **tsugumi-kit** | The repository of the eight default language SDKs (C++, Python, Go, Rust, C#, Julia, R, Ruby) and example templates, pinned by Tsugumi |
| **Template** | A folder of Card definitions to copy into a Workflow, from the project's `templates/` or the host's store ([Templates](design/CoreAndSDK.md#templates)) |
| **Card ABI** | The contract between the core and a language SDK: `tsugumi_main` in `lib/main.so` plus `card.fbs`, version 0 while unstable ([Card ABI](design/CardABI.md)) |
| **Worker** | The core's process that loads a Card's `lib/main.so` and calls it. Can stay resident across runs with `reuseWorker` ([The sandbox contract](design/SandboxContract.md#the-unit-of-isolation-is-the-card)) |
| **bubblewrap / Seatbelt** | How Cards are isolated: bubblewrap inside the VM and on Linux, Seatbelt in a Web UI run directly on a Mac from the repository |
| **nixPath** | Named Nix sources declared in `tsugumi.json` and resolved by Cards through `<name/path>` |
| **tsugumi.lock** | The project document that maps each Nix source name to a URL pinned by revision and NAR hash |
| **params.nix** | A Card definition file with a flat set of literals that hold the current parameter values and determine their types; a Card without params has none |

## Agent

| Term | Meaning |
| --- | --- |
| **Agent** | A chat connected to a model that makes and runs Cards with tools derived from the RPC ([the Agent in Features](design/Agent.md)) |
| **Helper** | A child Agent the Agent uses to split up independent work |
| **The thrush (Tsugumi)** | The bird of the chat and the guide. Flies around the screen to explain |
| **Approval (`approval_required`)** | Asking the user to confirm when an Agent's save widens a Card's permissions ([The Agent's permission boundary](design/security.md#the-agents-permission-boundary)) |
| **Skill** | A bundle of instructions called with `$` |
