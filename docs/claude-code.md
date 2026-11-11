# Using it from Claude Code

You can open a Tsugumi project from Claude Code and leave it to make, edit and run Cards and check the results (the Claude Code plugin `tsugumi`, `plugins/tsugumi/`). It has been verified on a Mac and on Windows (WSL2).
The work shows in the [Web UI](WebUI.md) as it happens: Cards Claude Code saves change on the Canvas, runs show their progress, and the thrush flies next to the Card Claude Code is working on. You watch the same screen and can work on it yourself.

## How it works

```text
Claude Code ── MCP (stdio) ── web/dist/mcp.js ── HTTP /api/agent/* ── Web UI server ── tsugumid
                                                                       └─ events ── browser (Canvas, thrush, approvals)
```

- The plugin's MCP server (`web/server/mcp.ts`) does not connect to `tsugumid` directly. It asks the Web UI's server, which runs the same tools as the chat's Agent (`project_inspect`, `project_save`, `run`, `run_record`, `log_range` and the rest) with the same checks (argument validation, `op_id`, revision conflicts, waiting for runs). The `origin` of its saves is `agent`.
- The server also tells the page about each call (`agentWork`). As with the chat's Agent, the page flies the thrush to that Card and adds a word such as "Reading generate's record". After about ten seconds without calls, the thrush returns to the top left of the chat. The thrush appears while Claude Code works even if no model key is registered in Tsugumi.
- The run animation (Card states, values flowing along wires) comes from `tsugumid`, so it looks the same whoever runs it.
- A save that newly gives a Card the network, running without isolation or a host folder waits for the user's approval, as for the chat's Agent. The approval appears in the Web UI's chat as "Claude Code wants to widen a Card's permissions". Without an open Web UI page there is no way to approve, so the save is not made, and Claude Code is told to explain this to the user.
- Reading a folder attached to the chat (`attached_folder_read`), handing work to helpers (`delegate`) and the thrush's speech bubble (`guide_show`) belong to the chat and are not given to Claude Code, which uses its own file operations and subagents.

## Setup

1. Keep a copy of the Tsugumi repository and build the core and the Web UI as in [Building and starting the Web UI](WebUI.md#building-and-starting).

2. In Claude Code, add this repository as a marketplace and install the plugin:

   ```text
   /plugin marketplace add gen740/Tsugumi
   /plugin install tsugumi@tsugumi
   ```

   When installing, give the folder of the repository you built in step 1 as "Tsugumi checkout" (change it later with `/plugin configure tsugumi@tsugumi`). The plugin is copied into Claude Code's cache, which does not contain what you built, so the MCP server is started from this folder. If `node` is not found, the `node` of that folder's Nix development shell (`nix develop`) is used.

## Using it

- Use `/tsugumi:open <project folder>`, or ask "in this Tsugumi project, do …". Claude Code opens the project with `tsugumi_open` before working.
- `tsugumi_open` looks for a running Web UI server (`server.json` in the data folder) and starts one if there is none (it keeps running after Claude Code closes; stop it by stopping that process). If a page is showing that project, or a Home page is open, the project is shown there (a page showing another project is not switched); otherwise the server opens it in a browser (because the key in `server.json` is not the page's key; [Security §8](security.md#the-agents-permission-boundary)).
- A new project can be made with `create: true` on `tsugumi_open` (only for a folder that does not exist yet).
- The first `tsugumi_open` returns the rules for using the tools (`agent/Shared/system-prompt.md`, the same as the chat's Agent).

| Environment variable (MCP server) | Meaning |
| --- | --- |
| `TSUGUMI_WEB_DATA` | The Web UI's data folder (where to look for `server.json`). Use the same value as the server |
| `TSUGUMI_WEB_PORT` | The port of a server it starts (by default, a free port) |

## What is not supported

- The macOS app is not supported. Projects Claude Code works on are opened in the Web UI.
- Cards run in the environment of the Web UI server `tsugumi_open` found (inside the shared VM if the `tsugumi` command's server is running on a Mac). If no server is running, a server is started from the repository you built, and `tsugumid` runs directly on this host.
