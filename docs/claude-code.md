# Using it from Claude Code

You can open a Tsugumi project from Claude Code and leave it to make, edit and run Cards and check the results (the Claude Code plugin `tsugumi`, `plugins/tsugumi/`).
The work shows in the Mac app, or in the [Web UI](WebUI.md), as it happens: Cards Claude Code saves change on the Canvas, runs show their progress, and the thrush flies next to the Card Claude Code is working on. You watch the same screen and can work on it yourself.
Codex uses the same MCP server ([Using it from Codex](codex.md)).

## How it works

```text
Claude Code ── MCP (stdio) ── tsugumi mcp ── Agent RPC (external) ── tsugumi-hostd ── tsugumid
                                                                       ├─ Mac app windows (Canvas, thrush)
                                                                       └─ Web UI ── browser (Canvas, thrush, approvals)
```

- The MCP server (`tsugumi mcp`, `cli/src/mcp.cc`) calls `tsugumi-hostd` on its Agent socket as the external-agent principal. The service validates tool arguments, applies saves with `origin: agent` and follows runs through the project RPC.
- Agent events and project notifications let the app and the Web UI show the work and run progress. A model key in Tsugumi is not required for Claude Code to use the tools.
- Connecting the plugin is your go-ahead: Claude Code reads, changes and runs the open project and uses its shell without a prompt. A save that widens a Card's permissions (network, host folders, a host-kept secret, looser isolation) waits for your decision. Deleting run history, clearing the build cache, version changes, lock updates and adding templates are done when you ask. The call returns `decision_required` naming what waits; the request stays open and the app or the Web UI shows it on the project, also when one opens later. A disconnected page does not cancel a core conversation. [Agent permission boundary](design/security.md#the-agents-permission-boundary)
- The MCP server exposes hostd's tool catalog and Agent API operations prefixed with `agent_` (described from the embedded `agent.bfbs`, `cli/src/agent_catalog.cc`). Claude Code can also use its own file tools and subagents; their permissions belong to Claude Code.

## Setup

The MCP server is the `tsugumi` command. The Mac app carries it at `Tsugumi.app/Contents/Helpers/tsugumi`; a built checkout has it at `build/cli/tsugumi`.

1. Install the Mac app, or keep a copy of the Tsugumi repository and build the core and the Web UI as in [Building and starting the Web UI](WebUI.md#building-and-starting).

2. In Claude Code, add this repository as a marketplace and install the plugin:

   ```text
   /plugin marketplace add gen740/Tsugumi
   /plugin install tsugumi@tsugumi
   ```

   The plugin starts the first of these that exists: `/Applications/Tsugumi.app/Contents/Helpers/tsugumi`, `/Applications/Tsugumi Dev.app/Contents/Helpers/tsugumi`, then `build/cli/tsugumi` of the checkout given as "Tsugumi checkout" when installing (change it later with `/plugin configure tsugumi@tsugumi`). The plugin is copied into Claude Code's cache, which does not contain what you built, so a checkout is found through this option.

Without the plugin, register the app's command directly (**Settings → External Connections → Use from Codex / Claude Code** in the app shows it for the app you have open):

```bash
claude mcp add tsugumi -- "/Applications/Tsugumi.app/Contents/Helpers/tsugumi" mcp
```

## Using it

- Use `/tsugumi:open <project folder>`, or ask "in this Tsugumi project, do …". Claude Code opens the project with `tsugumi_open` before working.
- The command inside the Mac app reaches that app's host: the same state folder, startup lock, shared VM and settings as the app (`~/Library/Application Support/Tsugumi`, or `Tsugumi-dev` for Tsugumi Dev; the command reads the channel from the app's `Info.plist`). A window that shows the project, or one on Home, shows it; when none is free, the command opens the app with the project, behind the frontmost app. With the app closed, the command starts the host as the app would (`tsugumi-hostd --ensure` with the same arguments).
- A command from a checkout reaches the same host as `tsugumi web` (`tsugumi-hostd --ensure` on the same state folder and startup lock) and starts one if there is none. hostd's `ShowRequest` shows the project on a page that shows it or a Home page (a page showing another project is not switched); otherwise hostd opens it in a browser, so the page key never reaches Claude Code ([Security §7](design/security.md#the-agents-permission-boundary)).
- A host keeps running after Claude Code closes. Concurrent command, plugin and app starts use one host per state folder. A host that already runs keeps its build until it is stopped.
- A new project can be made with `create: true` on `tsugumi_open` (only for a folder that does not exist yet).
- The first `tsugumi_open` returns the MCP instructions (`hostd/agent/Shared/mcp-transport.md` and the shared rules, generated into `cli/src/mcp_instructions.hh`).

| Environment variable (MCP server) | Meaning |
| --- | --- |
| `TSUGUMI_HOST_STATE` | hostd's state folder. Use the same value as for `tsugumi web` |
| `TSUGUMI_SUPPORT_DIR` | The support folder (VM, store, host state), as for the app |
| `TSUGUMI_WEB_PORT` | The Web UI port of a host it starts (by default 8780, else a free port) |

## From a shell

The same tools are subcommands of the `tsugumi` command, generated from the catalog the MCP server offers (hostd's project tools and the `agent_*` operations of `agent.bfbs`), with the same external principal and access:

```bash
tsugumi --help                      # the tools and families
tsugumi run plan --help             # one tool's options, from its schema
tsugumi project inspect             # the project at or above this folder (or --project <folder>)
tsugumi run status --run-id <id>    # a run's status
tsugumi runs cancel --run-id <id>   # a family action (through the `tools` tool)
tsugumi agent access --project-id <id> --access Read
tsugumi call ConversationGetRequest '{"conversation_id":"…"}'   # one raw Agent request, for debugging
```

Options take each argument's name (`--run-id`); objects and lists of objects are JSON, and `--json '<object>'` gives all arguments at once. The result is printed as JSON; an error result exits with 1. A project tool first shows the project through `ShowRequest` without opening a window.

## Where Cards run

Cards run in the environment of the host `tsugumi_open` reached: inside the shared VM for the app's command, the Mac's installed `tsugumi` command, or a host the app started. A command from a checkout that starts a host itself runs `tsugumid` directly on this machine.

The plugin uses core's project-tool allow list in `hostd/agent/Shared/agent.json`,
shared with the built-in Agent. Run history deletion, cache clearing, version
checkout and summary replacement remain explicit UI actions. `project_shell`
runs in the project's VM workspace and reads the project without write access;
use `project_save` for edits and Cards for analysis. External reloads require
the user's approval for every Card permission widening, regardless of origin.
