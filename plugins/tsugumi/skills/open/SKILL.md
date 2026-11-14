---
name: open
description: Open a Tsugumi project (a folder with tsugumi.json) in the Tsugumi web UI and work on it with the Tsugumi tools. Use when the user asks to build, change, run or explain a Tsugumi workflow, Cards or a run.
argument-hint: "[project folder]"
---

Work on a Tsugumi project with the Tsugumi MCP tools.

1. Call `tsugumi_open` with the project folder: $ARGUMENTS
   - No folder given: use the current folder when it has a `tsugumi.json`, otherwise ask the user which project.
   - The folder has no `tsugumi.json` and the user wants a new project: pass `create: true` (the folder must not exist yet).
2. `tsugumi_open` opens the project in the Tsugumi web UI (it starts the web UI host when none runs) and, the first
   time, returns the rules for the other tools. Follow them: `project_inspect` first, saves with the `base_rev` you
   inspected, runs from the trigger Card, results read with `run_record` and `log_range`. Opening does not
   grant access: request the needed Read, Write or Run access through the `agent_` API and wait for the user
   before using project tools.
3. The user watches the web UI while you work: the Cards you save change on the canvas, runs show live, and Tsugumi
   (the bird) flies to the Card you work on. A save that widens what Cards may do (network, folders) waits for the
   user's approval there.
4. Report what you changed and ran by the Cards' names, and what the run showed.
