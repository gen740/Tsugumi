# Using it from Claude Code

You can open a Tsugumi project from Claude Code and leave it to make, edit and run Cards and check the results (the Claude Code plugin `tsugumi`, `plugins/tsugumi/`).
The work shows in the [Web UI](WebUI.md) as it happens: Cards Claude Code saves change on the Canvas, runs show their progress, and the thrush flies next to the Card Claude Code is working on. You watch the same screen and can work on it yourself.

## How it works

```text
Claude Code ── MCP (stdio) ── web/dist/mcp.js ── HTTP /api/agent/* ── Web UI server ── tsugumi-agentd ── tsugumid
                                                                       └─ events ── browser (Canvas, thrush, approvals)
```

- The MCP server (`web/server/mcp.ts`) asks the Web host, which forwards calls to `tsugumi-agentd` as an external-agent principal. The service validates tool arguments, checks access grants, applies saves with `origin: agent` and follows runs through the project RPC.
- Agent events and project notifications let the Web UI show the work and run progress. A model key in Tsugumi is not required for Claude Code to use the tools.
- Read, Write and Run access and permission-widening saves can require a user decision. Keep a Web UI page open to answer pending access requests. A disconnected page does not cancel a core conversation. [Agent permission boundary](security.md#the-agents-permission-boundary)
- The MCP server exposes agentd's tool catalog and Agent API operations prefixed with `agent_`. Use the Agent API to request project access before inspecting it. Claude Code can also use its own file tools and subagents; their permissions belong to Claude Code.

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
- `tsugumi_open` looks for a running Web UI server (`server.json` in the data folder) and starts one if there is none (it keeps running after Claude Code closes; stop it by stopping that process). If a page is showing that project, or a Home page is open, the project is shown there (a page showing another project is not switched); otherwise the server opens it in a browser (because the key in `server.json` is not the page's key; [Security §7](security.md#the-agents-permission-boundary)).
- A new project can be made with `create: true` on `tsugumi_open` (only for a folder that does not exist yet).
- The first `tsugumi_open` returns the MCP instructions defined by `INSTRUCTIONS` in `web/server/mcp.ts`.

| Environment variable (MCP server) | Meaning |
| --- | --- |
| `TSUGUMI_WEB_DATA` | The Web UI's data folder (where to look for `server.json`). Use the same value as the server |
| `TSUGUMI_WEB_PORT` | The port of a server it starts (by default, a free port) |

## What is not supported

- The macOS app is not supported. Projects Claude Code works on are opened in the Web UI.
- Cards run in the environment of the Web UI server `tsugumi_open` found (inside the shared VM if the `tsugumi` command's server is running on a Mac). If no server is running, a server is started from the repository you built, and `tsugumid` runs directly on this host.

The plugin uses core's project-tool allow list in `agent/Shared/agent.json`,
shared with the built-in Agent. Run history deletion, cache clearing, version
checkout and summary replacement remain explicit UI actions. `project_shell`
requires a separate Shell grant and reads the project without write access;
use `project_save` for edits and Cards for analysis. External reloads require
the user's approval for every Card permission widening, regardless of origin.
