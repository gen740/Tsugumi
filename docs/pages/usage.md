# Usage

You do not need to read this first. Open a demo and **follow the thrush's guide, and you will mostly learn how the screen works**.
If something is unclear, ask in the chat on the right. The Agent knows the open project and the operations on screen.

## The first five minutes

The steps are in the README's [First steps](../index.md#first-steps). The first time you open the app, after entering an API key (which you can skip), the thrush in the bottom right asks "What would you like to do?" and takes you to a demo chosen by field or from the Drafts.

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

## To learn more

- Make your first Card yourself: [Your first Card](cardguide.md)
- What each demo does and what you can try: [The bundled demos](demos.md)
- How the screen is organised, and the settings: [the UI in Features](features.md#8-ui-the-workbench)
- Installation, and how API keys are handled: [Installation and development builds](install.md), [What is sent to the model](security.md#what-is-sent-to-the-model)
