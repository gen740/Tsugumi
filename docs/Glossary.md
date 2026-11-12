# Glossary

The words used in Tsugumi's documents and screens. See each row's link for details.

## Assembling processing

| Term | Meaning |
| --- | --- |
| **Workflow** | A graph of Cards connected by wires. The same thing a person draws, the Agent edits and the runtime executes ([Rationale](Rationale.md)) |
| **Card** | A unit of processing, with input and output ports and settings ([Cards and Workflows](features.md#2-cards-and-workflows)) |
| **Code Card** | A Card whose environment is defined in `default.nix` and whose processing is in `src/` (C++ / Python / bash / R / Julia; [Your first Card](CardGuide.md)) |
| **Built-in Card** | A Card without code, such as a Trigger, Input, Fork, Branch, Merge or Collect |
| **Draft** | An unimplemented Card with only a name and content. Wire Drafts up to draw a plan (their ports follow the wires) and have the Agent implement them |
| **Card in Card (child Workflow)** | A Card with a Workflow inside. From outside it looks like one Card; open it to go inside |
| **Definition** | The folder `definitions/<id>/` that is a Card's substance. One per Card |
| **Port** | A Card's input or output. Output types are written in `_outputs.fbs`; input types are derived from the connected outputs |
| **`all` / `any`** | How inputs are awaited. `all` takes one value from each once all required inputs have arrived; `any` consumes them in order of arrival ([Execution semantics](features.md#execution-semantics-scheduler)) |
| **`maxRuns`** | The most times a Card can be triggered in one Run. A trigger can start multiple invocations when it uses a run list or receives a batch |

## Running and recording

| Term | Meaning |
| --- | --- |
| **Run** | One execution of a Workflow. Records the version (commit) at its start |
| **Invocation** | One execution of a Card within a Run. In a loop, the same Card is invoked many times |
| **Batch** | An ordered set of values sent together on one wire. A run list or batch input can produce several invocations for one trigger |
| **Stage** | A step within an invocation: SDK preparation, Nix evaluation, build, run, output validation, saving ([Stages](features.md#stages)) |
| **`skipped`** | Downstream of a branch that was not taken. Not a failure |
| **`blocked`** | A Card that cannot run because it depends on a Draft or a failed upstream Card. Comes with a reason |
| **`interrupted`** | A run that was in progress when the process ended abnormally or restarted |
| **Partial run (single run)** | Running only the chosen Cards, or "from here". Uses saved inputs ([Rerunning from the middle](Rationale.md#rerunning-from-the-middle)) |
| **Saved inputs** | The full set of inputs of the latest trigger, including any batches. Partial runs use these inputs independently of `recordIO` |
| **`recordIO`** | A setting that keeps the input and output values of every invocation of a Card in the history (off by default) |
| **`retainArtifact`** | A setting that keeps a Card's artifacts |
| **Artifact** | A file or folder a Card output |
| **Display** | The result a Card shows on screen: text, a table, an image, a chart and so on ([Visualisation](features.md#7-visualisation)) |
| **Version** | A commit of the project's git. A named version is an annotated tag ([Versions](Versions.md)) |

## The runtime

| Term | Meaning |
| --- | --- |
| **`tsugumid` (the core)** | A process, one per project, that handles the document, the scheduler, Nix evaluation and history |
| **RPC** | The contract by which the UI, the Web and the Agent talk to `tsugumid` ([Protocol.md](Protocol.md)) |
| **`op_id`** | The ID of a request that changes something. A resend with the same ID is applied only once |
| **rev** | The document's revision. A save is based on it, and a save based on an old one is refused as a conflict |
| **Shared VM** | The one Linux VM that the Mac app and the Mac's `tsugumi` command share. Only one of them can use it at a time (`vm.lock`; [Security and isolation](security.md)) |
| **`tsugumi-vmd`** | The managing process inside the VM. Starts each project's `tsugumid` with its own uid, namespaces and cgroup |
| **Worker** | The process that runs a Card's code. Can stay resident across runs ([Resident workers](features.md#resident-workers)) |
| **bubblewrap / Seatbelt** | How Cards are isolated: bubblewrap inside the VM and on Linux, Seatbelt in a Web UI run directly on a Mac from the repository |
| **pkgsPath** | The nixpkgs that Cards use, pinned (`tsugumi.json`) |

## Agent

| Term | Meaning |
| --- | --- |
| **Agent** | A chat connected to a model that makes and runs Cards with tools derived from the RPC ([the Agent in Features](features.md#9-agent)) |
| **Helper** | A child Agent the Agent uses to split up independent work |
| **The thrush (Tsugumi)** | The bird of the chat and the guide. Flies around the screen to explain |
| **Approval (`approval_required`)** | Asking the user to confirm when an Agent's save widens a Card's permissions ([The Agent's permission boundary](security.md#the-agents-permission-boundary)) |
| **Skill** | A bundle of instructions called with `$` |
