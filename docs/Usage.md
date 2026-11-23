# Usage

You do not need to read this first. Open a demo and **let the thrush point out where to run it, view its results and try a parameter change**.
If something is unclear, ask in the chat on the right. The Agent knows the open project and the operations on screen.

## The first five minutes

The steps are in the README's [First steps](../README.md#first-steps). The first time you open the app, after entering an API key (which you can skip), the thrush in the bottom right asks "What would you like to do?" and takes you to a demo chosen by field or from the Drafts.

## Creating and copying projects

Choose **New Project** on Home and select a destination. A new folder is created if it does not exist; an existing folder must be completely empty, including hidden files. A non-empty folder is refused without overwriting its contents. Folder names cannot be empty, `.` or `..`, contain separators, control characters or `< > : " | ? *`, start or end with a space, or end with a dot.

**Open a Copy** on a bundled demo and **Duplicate Project** make writable copies of saved files. Copies omit `.tsugumi` run history, Agent conversations and state, `.git` repository identity and symbolic links; they start as independent projects.

## The screen

| Where | What is there |
| --- | --- |
| Centre (Canvas) | Cards, wires and notes |
| Right (chat) | Requests and questions to the Agent (the thrush) |
| Left (sidebar) | The hierarchy of Workflows and the list of Cards |
| Bottom (panel) | The run's state, logs and the timeline |

## Operations you use most

| To | Do |
| --- | --- |
| Run or stop | The buttons at the top of the Canvas |
| Place a Draft | Press `E` and write its content on the card (a name is optional) |
| Place a Card | Right-click the Canvas and choose "Add Card" |
| Connect | Drag from an output port or a Draft’s outgoing edge; drop on an input port or anywhere on a Draft |
| See or fix what is inside a Card | `s`, or "Settings" in the context menu (source, ports, Nix definition, run history) |
| Run only this Card | "Run Only This Card (Last Inputs)" in the context menu |
| Run from this Card downstream | "Run from Here Downstream" in the context menu |
| Leave implementation to the Agent | "Generate Cards", at the top right when there are Drafts |
| Go inside a child Workflow | Double-click |
| Move and zoom | Drag to pan; pinch or `⌘` + scroll to zoom |
| Duplicate, select a range | `c`; `Shift` + drag |
| Go back Home | The house button at the top right of the Canvas, or `⌘⇧H` (after any save that is needed; not while running) |

## Reuse Cards from a template

Choose **Add Card → From Template…** on the Canvas or in the Card definitions window. Project and Host templates appear in separate sections. Choose one entry to insert its Cards with fresh IDs and internal connections. A template copied twice produces two independent sets of Cards. Undo removes the whole copy.

Use **Add Template…** to select a folder on the host, or **Import from Git…** to enter a public HTTPS Git repository, a branch, tag or commit, and a template subdirectory. Choose a name and a destination store. A Project template is versioned under the project's `templates/` directory. A Host template is kept under `templates/` in the Host support folder and is available across projects on that host. The Agent can list and copy stored templates. Its requests to add or import require your approval.

A template folder contains one directory per Card, each with the original Card definition and sources. Put internal wiring in the optional `tsugumi.json` fragment using `canvas.connections`. Put positions in the optional `layout.bin` fragment using the `cards` entries defined in `protocol/layout_document.fbs`, keyed by the original Card ID. [Example folders and import values](https://github.com/gen740/tsugumi-kit/blob/main/docs/templates.md)

## Canvas tools

Right-click the Canvas and choose **Boolean Input** to create a toggle. Wire its value to a Code Card that chooses which optional outputs to produce. Use Merge to bring alternative outputs back to one downstream input. The timeline distinguishes skipped Cards. [Card types and execution](features.md#2-cards-and-workflows)


Use **Arrange** to place Cards and reduce wire crossings. Use **Untangle Wires** when the Cards are already where you want them. Both are available from the Canvas context menu and can be undone. Untangling changes the visual order of ports while preserving the connections and the way the Workflow runs.

Select several Cards and choose **Group Selection** to keep them together on the Canvas. To reuse another Workflow, use the import button by **Add Card** in the sidebar. It copies the selected project into `externals` as a Card in Card. Its boundary maps the outer ports to steps inside, which you can open by double-clicking the Card.

Use **Nix references** in the Canvas controls to see the upstream Card exports available to each Card through its input ports. Hover a Card to highlight its links or open **List** to jump to either end or open the consumer's `default.nix`. An input port named `data` is available as `inputs.data` when its producer is known. [Nix reference rules](features.md#sharing-nix-exports-through-connections)

## Run from a Card

Run the Workflow once to save its inputs. For example, open a copy of **Visualization → report**, run it, and change the aggregate Card's `offset`. Choose **Run from Here Downstream** on that Card to update the figures and report without running unrelated steps. Choose **Run Only This Card (Last Inputs)** to test just one figure using its saved input. A Card that needs an input without a saved value reports it as missing.

Use the bottom **Timeline** to see which steps ran. Select a Card there to inspect its log, or open **Card settings → Run History** to choose an earlier invocation. [Run history](RunHistory.md)

A thick wire carries a batch of values. Its count shows how many items are in that batch. Connect it to a Collect Card to pass one vector or Directory to the next step. [Run lists and Collect](features.md#run-lists-and-collect)

## Live displays

Right-click a Code Card and choose **Open Display**. The window shows the views
the Card declares in its `default.nix`, so it can be opened before the first run,
and each view follows what the Card sends to it while it works. A view is live:
nothing a Card sends is recorded, so it cannot be reopened from Run History,
compared between runs or cited in chat. Keep a result to check later as an
output value, a file or an artifact. [Views](CardGuide.md#views)

## Project terminal

Open the **Terminal** panel below the Canvas. Use **New Terminal** to open another shell and run a command such as `nix --version`. A project can have up to eight terminal tabs. Switching tabs or hiding the panel keeps their commands running. Closing a terminal ends its shell. Leaving a project in the Web UI ends its terminal tabs; stopping the environment ends project shells in the VM.

On Mac, the app and the installed browser command use the project's environment inside the shared Linux VM. The Linux command runs on the host and the Windows command runs under WSL2. The Agent's shell tool has its own permission setting. [Browser and CLI environments](WebUI.md#the-tsugumi-command) · [Agent shell access](security.md#the-agents-permission-boundary)

## To learn more

- Make your first Card yourself: [Your first Card](CardGuide.md)
- What each demo does and what you can try: [The bundled demos](Demos.md)
- How the screen is organised, and the settings: [The screen](#the-screen)
- Installation, and how API keys are handled: [Installation and development builds](INSTALL.md), [What is sent to the model](security.md#what-is-sent-to-the-model)

## Manage build and scratch space

Open **Settings → project → Build Cache** in Mac or Web. Review the shared total, per-project build cache, last root use and each project's total run scratch. **Delete** beside an open project removes its build registrations; open a closed project first to use that action. **Clean shared store** in the Mac app's External Connections settings then removes unrooted store paths. Shared dependencies can stay because another project still keeps them. For this project, select individual past scratch folders and review the selected total, then choose **Delete selected scratch…**. A folder whose size cannot be measured shows **Unknown**; the selected total excludes unknown sizes. The latest run stays available for reruns; older scratch remains until a person deletes it, including after restarting.

Opening this Settings pane measures build and scratch usage. Build sizes use NAR estimates; scratch sizes sum file lengths without following links. Errors and unknown sizes are shown explicitly. There is no background polling or large-cache notice. For scratch lifetime and retained copies, see [Run history](RunHistory.md#copy-or-delete-history).

## Card settings and app settings

Card names appear on the Canvas and in lists, history, comparisons and chat references. Renaming changes the name only; tools, file paths and connections keep the permanent identifier. A Card without a name shows its identifier.

Open a Card's settings to edit its working copy, then choose **Apply**. A parameter edit rewrites its current literal in `params.nix` and preserves the surrounding source. Settings JSON, Card settings and project settings use the same revision check. Inline value edits use the revision at which editing began; leaving an unchanged value does not save it. Text entered while Apply or Save is pending stays unapplied after the submitted text is saved. If another save conflicts, choose **Overwrite** or **Keep edits and reload**; the latter keeps your previous text available to reapply. Tabs include Source, Ports, Comments, Nix Definition, Run History and Artifacts; the available tabs depend on the Card's kind. Run history and artifacts remain readable while running, but changes to Cards and closing a running project are blocked.

Use **Reload External Changes** under save/sync to import edits made outside Tsugumi. If those edits widen Card permissions, review the listed permissions and choose **Allow** to finish reloading or **Decline** to leave the loaded project unchanged.

Use the app settings for language, appearance, model connections, Skills and shortcuts; use project settings for its environment, Canvas, history and Build Cache. Choose Japanese or English under **General** (Japanese by default); restart the Mac app to apply it, while the Web UI reloads itself. Signed [binary caches](BinaryCaches.md) apply at the next Mac VM start.

## Several projects

The Mac app shows one project or Home per window. **⌘⇧N** adds a Home window; **⌘N**, **⌘O**, demos and duplicates open in a new window. Opening an already open project brings its window forward. **⌘W** saves and closes that project; **⌘Q** checks unsaved changes across windows. The shared VM supports up to 16 open projects.

Closing a project window leaves its execution environment resident. Stop it explicitly through the environment controls when it should end. Workflows, history, chats and selections are separate; API keys, app settings and the shared VM/store are shared. The Web UI uses [one project per browser tab](WebUI.md#several-projects). Only one UI connection can open each project at a time.

## Working with the Agent

**Project settings → General → Paths excluded from the Agent** edits `agentExclude` in `tsugumi.json`: one project-relative or absolute path per line. Each entry hides a file or its entire tree from Agent tools and helpers; symbolic links are compared on resolved real paths. Lists omit excluded content and direct access returns `not_found`. People retain the complete project view. The absent setting defaults to `.git` (repository internals) and `.DS_Store` (Finder metadata); the settings editor shows these editable entries. An explicit empty list disables the defaults. `.tsugumi` contains the person's run records, conversations and scratch, so it is not a default exclusion. Only a person can change this setting; Agent saves and edits cannot change it.

Use `project_copy` to copy a bundled demo Card with `source: "demo"`, a language-qualified demo path, the source `card` ID and the destination `id`. The copy includes its definition and source files. Add any required dependencies to the destination's [Nix search path](features.md#project-nix-search-path) and update its lock.

Drafts have no ports; any number of wires can attach to the Card itself. To implement one by hand, choose **Implement Draft by hand** in Card settings → General, then specify every real endpoint under **Ports → Rebind wires** and Apply. Even a single candidate must be stated. The definition and all endpoint choices are saved together.

To implement a plan, describe Drafts, connect their wires and choose **Generate Cards**. Review the saved source and each Card's run record. On failure or cancellation, completed saves remain; the save in progress is not applied. Saves widening permissions require [approval](security.md#the-agents-permission-boundary).

To request a change, open **Comments…** on a Card (or its badge), write the request and choose **Generate from comments**. Selected Cards with open comments take precedence; otherwise the button uses all open comments at the current level. Resolved comments are folded and can be reopened. The save row and [version history](Versions.md#comments) show who changed them.

Send **$demo** in a project's chat for a short tour: run, open the result, and optionally change a parameter, Apply and run again. You can also rerun with unchanged settings to explore reproducibility. The thrush points at controls and result locations; you read and judge the results, without a checklist to confirm. An action runs only when you press its button. **⌥⌘J** opens or closes the chat. Ask for more detail in the ordinary chat.

### Agent activity

The Canvas bird and its speech bubble show current work. File and patch previews remain marked **Not saved** until the save succeeds. On Mac, choose a thrush, a dot or nothing under **Appearance → The Agent at work**.

Turn on **Draw thoughts** below the chat input for model-written activity summaries. It asks for consent and shows separate token usage; turning it off restores the activity line. Review actual saved files and results before judging progress.

### Skills and conversations

Add Skill folders in settings or keep Skills under the project's `skills/`; call one with **$name**. Use the conversation list to search past chats or compact a conversation. Turn on **Web search** under **API** only when you want the provider to receive search terms. [Search and privacy](security.md#web-search)

Project conversations are saved in `.tsugumi/agent.sqlite`, outside git, alongside the project's run records. Copy the whole project folder, including `.tsugumi/`, to keep what the Agent said and did and the records linked in its replies. Stop the Agent service before copying. Home conversations stay on the host. API keys, provider settings and access grants stay on the host too: approve project and attached-folder access again on another machine. The Mac app, Web UI and plugin use the same Agent service. Open a copied project to see its saved chats in the conversation list and search. Deleting a conversation removes it from its project; resetting host state keeps project conversations. Another Agent host cannot write the same project's conversations until the first service stops. [Storage and deletion](security.md#8-where-data-is-kept-and-how-to-delete-it)

An interrupted conversation stays interrupted when opened, refreshed, reconnected or after a service restart. The chat shows the operation recorded by the core at interruption (including the tool call and its arguments, when applicable). Choose **Resume** to continue, or send a new message. Resuming reconciles in-flight tool calls against their durable receipts; it does not blindly run them again. Work still running in the core can be observed from another window without resuming it.

### Canvas presentation

Card and note positions change when you move them, choose **Arrange**,
or ask the Agent to change the layout. Showing the Agent's saves or run outputs
never rearranges the canvas. A new Card is placed when it is created.

**Settings → Appearance → Animate the Agent’s work** is on by default and is stored
in hostd with the host's Agent settings (shared by the Mac app and its Web UI).
On the Mac, it stages saved Cards becoming code and threads their wires in sequence.
Turn it off to show saved Cards and wires immediately, while the work marker follows
the latest activity. The Web UI already shows saved Cards and wires immediately;
this setting also controls whether its helpers' work markers wait for queued scenes.
The bird's visibility and motion, **Draw thoughts**, and source text streaming as
it is written have separate controls or behaviour.

The Mac app opens directly to Home. Recent projects, demos and project defaults
load independently; lists show a loading indicator until their first reply. The
host setup check runs alongside Home and shows missing prerequisites only when
confirmed. A failed check shows an error, with a retry, rather than claiming that
the Mac or VM image is unavailable. VM preparation starts when a project opens.
