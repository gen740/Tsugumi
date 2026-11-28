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
   inspected, runs from the trigger Card, results read with `run_record` and `log_range`.
3. The user watches the web UI while you work: the Cards you save change on the canvas, runs show live, and Tsugumi
   (the bird) flies to the Card you work on. A save that widens what Cards may do (network, folders, secrets) waits
   for the user's approval in Tsugumi; the call returns `decision_required`, and after they answer you call it again.
   The returned rules describe current values in each Card's `params.nix`, the explicit `nix_path_lock_update` operation, generated `_inputs.fbs`, and how to join sources with Merge. Use `inputs = ./_inputs.fbs;` in language SDK builder calls, including for Cards without connected inputs. Build generated SDK declarations with `evaluate_nix` on the local system, using `(import ./. { inherit params inputs; }).api` and `build: true`.
4. Use Card ids and port names matching `[a-z][a-z0-9_]{0,63}` in tools. Report what you changed and ran by the Cards' README.md titles, and what the run showed.
