# Using it from Codex

Codex can open a Tsugumi project, make and edit Cards, run them and read the results through Tsugumi's MCP server (`tsugumi mcp`). The server is the `tsugumi` command inside the Mac app, so nothing else needs to be installed or built. The work shows in the app as it happens: Cards Codex saves change on the Canvas, runs show their progress, and the thrush flies next to the Card Codex is working on.

The same server is what the Claude Code plugin starts ([Using it from Claude Code](claude-code.md)); everything on this page about the host and the app holds for both.

## Setup

Register the command of the app you use with Codex:

```bash
codex mcp add tsugumi -- "/Applications/Tsugumi.app/Contents/Helpers/tsugumi" mcp
```

For Tsugumi Dev, use `/Applications/Tsugumi Dev.app/Contents/Helpers/tsugumi`. **Settings → External Connections → Use from Codex / Claude Code** in the app shows this command with the path of the app you have open, and copies it.

`codex mcp list` shows the registered server. `codex mcp remove tsugumi` removes it.

Codex asks before each call of an MCP tool unless you allow the server's tools. For `codex exec`, which cannot ask, allow them in `~/.codex/config.toml`:

```toml
[mcp_servers.tsugumi]
default_tools_approval_mode = "approve"
```

## Opening a project

Ask Codex to work on a Tsugumi project folder (a folder with `tsugumi.json`). It calls the `tsugumi_open` tool with the folder before working; `create: true` makes a new project in a folder that does not exist yet. The first `tsugumi_open` returns the rules for the other tools.

- `tsugumi_open` reaches the app's own host: the same state folder, startup lock, shared VM, settings and recent projects as the app (`~/Library/Application Support/Tsugumi`, or `Tsugumi-dev` for Tsugumi Dev). The command reads which app it is in from the app's `Info.plist`; Codex passes no option for it.
- When the app is open, a window that shows the project, or one on Home, shows it. When no window is free, the command opens the app with the project in a new window, behind the app you are using.
- When the app is closed, the command starts the host as the app would (`tsugumi-hostd --ensure`). The host keeps running after Codex ends; opening the app later connects to it.

## What shows in the app

- Saves Codex makes change the Canvas of the window showing the project, and the thrush moves to the Card being worked on.
- Runs Codex starts appear in the run list and on the Canvas with their progress, as runs you start yourself.
- Cards run in the app's shared Linux VM, as when you run them in the app.
- A Tsugumi model key is not needed: Codex uses its own model.

Codex's own file and shell tools, and their permissions, belong to Codex.

## Checking it from a shell

The same command runs one tool at a time, as the external principal Codex uses ([From a shell](claude-code.md#from-a-shell)):

```bash
"/Applications/Tsugumi.app/Contents/Helpers/tsugumi" project inspect --project <folder>
```
