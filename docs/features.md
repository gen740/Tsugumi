# Tsugumi's features

Tsugumi connects small, typed steps called Cards into a Workflow. Run the whole graph, change one step, and inspect the recorded conditions and results. Use the macOS app or open the workbench in a browser with the `tsugumi` command. [Installation](INSTALL.md) · [First steps in the workbench](Usage.md#the-first-five-minutes)

## Find a feature

### Build and navigate a Workflow

- **Sketch before writing code.** Add a Draft with `e` while the Canvas has focus, describe a step, and draw its wires. Add a Start, Merge or Collect Card when you already know how data should flow. [Canvas tools](Usage.md#canvas-tools) · [Kinds of Card](design/Workflow.md#kinds-of-card)
- **Organise a larger graph.** Right-click the Canvas and choose **Arrange** to place Cards or **Untangle Wires** to reduce crossings while keeping their positions. Group related Cards with **Group Selection**, or open a Card in Card to see its child Workflow and boundary ports. The import button by **Add Card** copies another project into `externals/` and places its Workflow as a Card in Card. [Canvas tools](Usage.md#canvas-tools) · [Cards and Workflows](design/Workflow.md)
- **Run variants and batches.** Give a Card a list of implementations in `run`, or send it a batch, and it runs every combination. Connect the batch to a Collect Card to pass one typed list of envelopes on. [Run lists and Collect](design/Workflow.md#run-lists-and-collect)

### Run, inspect and compare

- **Try one change.** Run the Workflow once, change a Card parameter, then choose **Run from Here Downstream**. Use **Run Only This Card (Last Inputs)** to try the step by itself with inputs saved from a previous run. [Run from a Card](Usage.md#run-from-a-card)
- **See what happened.** Use the bottom timeline or open **Card settings → Run History**. Pick an invocation and open **Ports**, **Log**, **Packages** or **Info** to see its inputs and outputs, its logs, and the stage at which it failed or stopped. [Inspect an invocation](RunHistory.md#inspect-an-invocation)
- **Compare results and check the code that produced them.** Select two runs in **Compare Runs** to compare their recorded parameters and values. Inspect the code and settings diff before choosing whether to revisit a saved version. [Compare two runs](RunHistory.md#compare-two-runs) · [Version history](Versions.md#the-version-tree)
- **Go back to an earlier version.** Open **Version History**, pick a version in the tree and choose **Start from This Version**. [The version tree](Versions.md#the-version-tree)
- **Check what an invocation ran with.** Open **Packages** in its run record and search the recorded runtime store paths. Open an evidence link from the Agent's chat to inspect a cited run, log or artifact. [Runtime packages](RunHistory.md#find-the-runtime-packages) · [Evidence links](RunHistory.md#follow-evidence-from-chat)

### Display results and keep files

- **Plot data with editable code.** Write a Code Card with Matplotlib or Plotly, show the figure live in one of the Card's views and save it as an Artifact output. For example, plot a numeric column as a histogram and keep the actual bins with the figure. [Visualisation](Visualization.md) · [Views](CardGuide.md#views)
- **Watch a running Card.** Right-click a Code Card and choose **Open Display** to see the views it declares and follow what it sends while it works. Views are live: what a Card sends is not recorded, so a result to check later goes to an output or an artifact. [Live displays](Usage.md#live-displays) · [Views](CardGuide.md#views)
- **Run a notebook or render a report.** Add a marimo notebook to run committed cells once and return typed results. Use an R Markdown or Quarto Card to render a parameterised HTML report from an upstream value. [marimo notebooks](Marimo.md) · [R Markdown and Quarto](Reports.md)
- **Keep a result for later.** Return an Artifact and open its files under **Artifacts**, including a static preview for HTML. [Keep and open artifacts](RunHistory.md#keep-and-open-artifacts)

### Write Cards and pin environments

- Choose a Code Card’s language SDK in `default.nix`. The SDKs for C++, Python, Go, Rust, C#, Julia, R, Ruby, Java and JavaScript come from [tsugumi-kit](https://github.com/gen740/tsugumi-kit). Start with a Draft or choose **From Template…** to copy saved Cards. [First Card](CardGuide.md) · [SDK entry points](design/CoreAndSDK.md#the-builder-interface) · [Templates](CardGuide.md#templates)
- **Pin dependencies and control state.** Use **Project settings → Name and Environment** to declare and lock project sources. Declare a Card-local lock where the SDK supports one (Python, Ruby, Go, Rust and C#). For a Card that needs interpreter or library state, turn on **Keep the worker's state** (`reuseWorker`) in its settings. [Nix environments](design/Nix.md) · [Language SDKs](CardGuide.md) · [Worker state](CardGuide.md#worker-state)
- **Keep a monitoring worker resident.** Workers retained with `reuseWorker` have no count, idle-time or memory eviction limit. They stay until the environment stops or the Card's definition or execution conditions change; a failed, cancelled or timed-out call discards its worker, and a dead worker is replaced. Each invocation records reuse, and its log identifies replacement of resident state. Fresh workers prepared for speed keep their separate limits. [Worker state](CardGuide.md#worker-state)
- **Use a shell or a signed build cache.** Open the project Terminal to run a tool such as `nix --version`. On Mac, configure a shared signed cache in **Settings → Binary Caches** and apply it on the next VM start. [Project terminal](Usage.md#project-terminal) · [Binary caches](BinaryCaches.md) · [Browser and CLI platforms](WebUI.md#the-tsugumi-command)
- **Run a Card on another Linux machine.** Start `tsugumi-workerd` there, register it in **Settings → Execution service**, and inspect the selected worker in **Run History → Info**. [Run a Card on a remote worker](RemoteExecution.md) · [Execution service](design/RemoteWorker.md#worker-selection)
- **Control a Card's access.** Use **Card settings → General** to review **Restrict file access**, and the network and host folder permissions in the same tab, before running code from another project. [Security and isolation](design/security.md#summary)

### Work with the Agent and explore examples

- **Turn a Draft into a Card.** Connect a model under **Settings → API**, describe an operation in a Draft and choose **Generate Cards**. Review the generated source, ports and run record. Use Card comments to request a specific change. [Generate from Drafts](design/Agent.md#generating-cards-from-drafts) · [Generate from comments](design/Agent.md#generating-cards-from-comments) · [Data and permissions](design/security.md#7-the-models-api-keys-agent)
- **Start from an example.** Browse Demos and open a copy to change a parameter, run it again, or follow the thrush's guide. Use the [documentation site](https://gen740.github.io/Tsugumi/)'s recordings to inspect example Workflows before installing. [Bundled demos](Demos.md)
- **Work from Claude Code.** Install the plugin to have Claude Code make and run Cards through Tsugumi's tools, with the work shown in the Web UI. [Claude Code plugin](claude-code.md)

## Design and implementation

[Design index](design/README.md) for the architecture and contracts.
