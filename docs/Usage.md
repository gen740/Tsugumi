# Usage

You do not need to read this first. Open a demo and **follow the thrush's guide, and you will mostly learn how the screen works**.
If something is unclear, ask in the chat on the right. The Agent knows the open project and the operations on screen.

## The first five minutes

The steps are in the README's [First steps](../README.md#first-steps). The first time you open the app, after entering an API key (which you can skip), the thrush in the bottom right asks "What would you like to do?" and takes you to a demo chosen by field or from the Drafts.

## Creating and copying projects

Choose **New Project** on Home and select a destination. A new folder is created if it does not exist; an existing folder must be completely empty, including hidden files. A non-empty folder is refused without overwriting its contents. Folder names cannot be empty, `.` or `..`, contain separators, control characters or `< > : " | ? *`, start or end with a space, or end with a dot.

**Open a Copy** on a bundled demo and **Duplicate Project** make writable copies of saved files. Copies omit `.tsugumi` run history and state, `.git` repository identity and symbolic links; they start as independent projects.

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

Open **Card settings → General → Chart updates**. The default **Update** shows the previous chart dimmed while the Card or an upstream Card is running. Choose **Live** to follow new chart revisions while the Card is running. Try both settings with the **Live Stream** demo. The Card must emit successive chart updates for the live setting to show changes. [Visualisation](features.md#7-visualisation)

## Project terminal

Open the **Terminal** panel below the Canvas. Use **New Terminal** to open another shell and run a command such as `nix --version`. A project can have up to eight terminal tabs. Switching tabs or hiding the panel keeps their commands running. Closing a terminal or closing the project ends its shell.

On Mac, the app and the installed browser command use the project's environment inside the shared Linux VM. The Linux command runs on the host and the Windows command runs under WSL2. The Agent's shell tool has its own permission setting. [Browser and CLI environments](WebUI.md#the-tsugumi-command) · [Agent shell access](features.md#commands-in-the-project-environment)

## To learn more

- Make your first Card yourself: [Your first Card](CardGuide.md)
- What each demo does and what you can try: [The bundled demos](Demos.md)
- How the screen is organised, and the settings: [the UI in Features](features.md#8-ui-the-workbench)
- Installation, and how API keys are handled: [Installation and development builds](INSTALL.md), [What is sent to the model](security.md#what-is-sent-to-the-model)
