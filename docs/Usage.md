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
| Centre (Canvas) | Cards, wires and the outputs' displays |
| Right (chat) | Requests and questions to the Agent (the thrush) |
| Left (sidebar) | The hierarchy of Workflows and the list of Cards |
| Bottom (panel) | The run's state, logs and the timeline |

## Operations you use most

| To | Do |
| --- | --- |
| Run or stop | The buttons at the top of the Canvas |
| Place a Draft | Press `E` and write its content on the card (a name is optional) |
| Place a Card | Right-click the Canvas and choose "Add Card" |
| Connect | Drag from an output port (dropped anywhere on a Draft, it connects) |
| See or fix what is inside a Card | `s`, or "Settings" in the context menu (source, ports, Nix definition, run history) |
| Run only this Card | "Run Only This Card (Last Inputs)" in the context menu |
| Run from this Card downstream | "Run from Here Downstream" in the context menu |
| Leave implementation to the Agent | "Generate Cards", at the top right when there are Drafts |
| Go inside a child Workflow | Double-click |
| Move and zoom | Drag to pan; pinch or `⌘` + scroll to zoom |
| Duplicate, select a range | `c`; `Shift` + drag |
| Go back Home | The house button at the top right of the Canvas, or `⌘⇧H` (after any save that is needed; not while running) |

## Canvas tools

Right-click the Canvas to add a Code Card, a Switch, a Fork, a Branch, a Merge or a Collect Card. A Switch can feed the `condition` input of a Branch so you can try both paths without editing code. Connect a value to the Branch's other input, then run once with the Switch on and once with it off. The timeline shows which branch ran. [Card types and execution](features.md#2-cards-and-workflows)

Use **Arrange** to place Cards and reduce wire crossings. Use **Untangle Wires** when the Cards are already where you want them. Both are available from the Canvas context menu and can be undone. Untangling changes the visual order of ports while preserving the connections and the way the Workflow runs.

Select several Cards and choose **Group Selection** to keep them together on the Canvas. To reuse another Workflow, use the import button by **Add Card** in the sidebar. It copies the selected project into `externals` as a Card in Card. Its boundary maps the outer ports to steps inside, which you can open by double-clicking the Card.

Use **Nix references (as written)** in the Canvas controls to see references between saved `default.nix` files. Hover a Card to highlight its references or open **List** to jump to a Card or the reference's source. The overlay shows directly written references such as `inputs.data`, where `data` is the Card's input port name. Aliases and expressions are resolved during evaluation. [Nix reference rules](features.md#sharing-nix-exports-through-connections)

## Run from a Card

Run the Workflow once to save its inputs. For example, open a copy of **Visualization → report**, run it, and change the aggregate Card's `offset`. Choose **Run from Here Downstream** on that Card to update the figures and report without running unrelated steps. Choose **Run Only This Card (Last Inputs)** to test just one figure using its saved input. A Card that needs an input without a saved value reports it as missing.

Use the bottom **Timeline** to see which steps ran. Select a Card there to inspect its log, or open **Card settings → Run History** to choose an earlier invocation. [Run history](RunHistory.md)

A thick wire carries a batch of values. Its count shows how many items are in that batch. Connect it to a Collect Card to pass one vector or Directory to the next step. [Run lists and Collect](features.md#run-lists-and-collect)

## Chart updates

Open **Card settings → General → Chart updates**. The default **Update** shows the previous chart dimmed while the Card or an upstream Card is running. Choose **Live** to follow new chart revisions while the Card is running. Try both settings with the **Live Stream** demo. The Card must emit successive chart updates for the live setting to show changes.

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

The core suggests reviewing this pane when build caches or run scratch occupy at least 20% of their disk and at least 1 GiB. The notice never deletes anything. Build sizes are cached NAR estimates; scratch sizes sum file lengths without following links. Measurements can take up to a minute to refresh; errors and unknown sizes are shown explicitly. For scratch lifetime and retained copies, see [Run history](RunHistory.md#copy-or-delete-history).

## Card settings and app settings

Card names appear on the Canvas and in lists, history, comparisons and chat references. Renaming changes the name only; tools, file paths and connections keep the permanent identifier. A Card without a name shows its identifier.

Open a Card's settings to edit its working copy, then choose **Apply**. Settings JSON, Card settings and project settings use the same revision check. Inline value edits use the revision at which editing began; leaving an unchanged value does not save it. Text entered while Apply or Save is pending stays unapplied after the submitted text is saved. If another save conflicts, choose **Overwrite** or **Keep edits and reload**; the latter keeps your previous text available to reapply. Tabs include Source, Ports, Comments, Nix Definition, Run History and Artifacts; the available tabs depend on the Card's kind. Run history and artifacts remain readable while running, but changes to Cards and closing a running project are blocked.

Use **Reload External Changes** under save/sync to import edits made outside Tsugumi. If those edits widen Card permissions, review the listed permissions and choose **Allow** to finish reloading or **Decline** to leave the loaded project unchanged.

Use the app settings for language, appearance, model connections, Skills and shortcuts; use project settings for its environment, Canvas, history and Build Cache. Choose Japanese or English under **General** (Japanese by default); restart the Mac app to apply it, while the Web UI reloads itself. Signed [binary caches](BinaryCaches.md) apply at the next Mac VM start.

## Several projects

The Mac app shows one project or Home per window. **⌘⇧N** adds a Home window; **⌘N**, **⌘O**, demos and duplicates open in a new window. Opening an already open project brings its window forward. **⌘W** saves and closes that project; **⌘Q** checks unsaved changes across windows. The shared VM supports up to 16 open projects.

Closing a project window leaves its execution environment resident. Stop it explicitly through the environment controls when it should end. Workflows, history, chats and selections are separate; API keys, app settings and the shared VM/store are shared. The Web UI uses [one project per browser tab](WebUI.md#several-projects).

## Working with the Agent

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

Card, output-frame and note positions change when you move them, choose **Arrange**,
or ask the Agent to change the layout. Showing the Agent's saves or run outputs
never rearranges the canvas. A new Card is placed when it is created.

**Settings → Appearance → Animate the Agent’s work** is on by default and is stored
in agentd with the host's Agent settings (shared by the Mac app and its Web UI).
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
