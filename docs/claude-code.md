# Using it from Claude Code

You can open a Tsugumi project from Claude Code and leave it to make, edit and run Cards and check the results (the Claude Code plugin `tsugumi`, `plugins/tsugumi/`).
The work shows in the [Web UI](WebUI.md) as it happens: Cards Claude Code saves change on the Canvas, runs show their progress, and the thrush flies next to the Card Claude Code is working on. You watch the same screen and can work on it yourself.

## How it works

```text
Claude Code ── MCP (stdio) ── tsugumi mcp ── Agent RPC (external) ── tsugumi-hostd ── tsugumid
                                                                       └─ Web UI ── browser (Canvas, thrush, approvals)
```

- The MCP server (`tsugumi mcp`, `cli/src/mcp.cc`) calls `tsugumi-hostd` on its Agent socket as the external-agent principal. The service validates tool arguments, checks access grants, applies saves with `origin: agent` and follows runs through the project RPC.
- Agent events and project notifications let the Web UI show the work and run progress. A model key in Tsugumi is not required for Claude Code to use the tools.
- Read, Write and Run access and permission-widening saves can require a user decision. Keep a Web UI page open to answer pending access requests. A disconnected page does not cancel a core conversation. [Agent permission boundary](design/security.md#the-agents-permission-boundary)
- The MCP server exposes hostd's tool catalog and Agent API operations prefixed with `agent_` (described from the embedded `agent.bfbs`, `cli/src/agent_catalog.cc`). Use the Agent API to request project access before inspecting it. Claude Code can also use its own file tools and subagents; their permissions belong to Claude Code.

## Setup

1. Keep a copy of the Tsugumi repository and build the core and the Web UI as in [Building and starting the Web UI](WebUI.md#building-and-starting).

2. In Claude Code, add this repository as a marketplace and install the plugin:

   ```text
   /plugin marketplace add gen740/Tsugumi
   /plugin install tsugumi@tsugumi
   ```

   When installing, give the folder of the repository you built in step 1 as "Tsugumi checkout" (change it later with `/plugin configure tsugumi@tsugumi`). The plugin is copied into Claude Code's cache, which does not contain what you built, so the MCP server is started from this folder (`build/cli/tsugumi mcp`).

## Using it

- Use `/tsugumi:open <project folder>`, or ask "in this Tsugumi project, do …". Claude Code opens the project with `tsugumi_open` before working.
- `tsugumi_open` reaches the same host as `tsugumi web` (`tsugumi-hostd --ensure` on the same state folder and startup lock) and starts one if there is none; it keeps running after Claude Code closes. hostd's `ShowRequest` shows the project on a page that shows it or a Home page (a page showing another project is not switched); otherwise hostd opens it in a browser, so the page key never reaches Claude Code ([Security §7](design/security.md#the-agents-permission-boundary)).
- Concurrent command and plugin starts use one host per state folder. A host that already runs keeps its build until it is stopped.
- A new project can be made with `create: true` on `tsugumi_open` (only for a folder that does not exist yet).
- The first `tsugumi_open` returns the MCP instructions (`hostd/agent/Shared/mcp-transport.md` and the shared rules, generated into `cli/src/mcp_instructions.hh`).

| Environment variable (MCP server) | Meaning |
| --- | --- |
| `TSUGUMI_HOST_STATE` | hostd's state folder. Use the same value as for `tsugumi web` |
| `TSUGUMI_WEB_PORT` | The Web UI port of a host it starts (by default 8780, else a free port) |

## From a shell

The same tools are subcommands of the `tsugumi` command, generated from the catalog the MCP server offers (hostd's project tools and the `agent_*` operations of `agent.bfbs`), with the same external principal and access:

```bash
tsugumi --help                      # the tools and families
tsugumi run plan --help             # one tool's options, from its schema
tsugumi project inspect             # the project at or above this folder (or --project <folder>)
tsugumi runs status --run-id <id>   # a family action (through the `tools` tool)
tsugumi agent access --project-id <id> --access Read
tsugumi call ConversationGetRequest '{"conversation_id":"…"}'   # one raw Agent request, for debugging
```

Options take each argument's name (`--run-id`); objects and lists of objects are JSON, and `--json '<object>'` gives all arguments at once. The result is printed as JSON; an error result exits with 1. A project tool first shows the project through `ShowRequest` without opening a browser.

## What is not supported

- The macOS app is not supported. Projects Claude Code works on are opened in the Web UI.
- Cards run in the environment of the host `tsugumi_open` found (inside the shared VM if the Mac's `tsugumi` command or the app started it). If no host is running, one is started from the repository you built, and `tsugumid` runs directly on this host.

The plugin uses core's project-tool allow list in `hostd/agent/Shared/agent.json`,
shared with the built-in Agent. Run history deletion, cache clearing, version
checkout and summary replacement remain explicit UI actions. `project_shell`
requires a separate Shell grant and reads the project without write access;
use `project_save` for edits and Cards for analysis. External reloads require
the user's approval for every Card permission widening, regardless of origin.
