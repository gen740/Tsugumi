# AGENTS.md

## The project's philosophy

The full text is in [docs/Philosophy.md](docs/Philosophy.md). In short:

- People ask the questions, give the results meaning and take responsibility. AI moves the trial and error forward (AI-driven science).
- The hard part is whether people can come back to judging after handing work over. As with Level 3 of automated driving, "a person checks at the end" means nothing if the process has been lost from sight.
- So keep the process, not a clean result and a long report. Split processing into pieces you can see through (Cards), make it possible to follow what went in, what was done and what came out, and let people change the conditions of a step they are curious about and run it again by their own hand.
- What a definition fixes (code and its environment) the machine may build once, share and reuse. What a run produced is for people to judge: do not add automation that keeps, reuses or deletes run data on its own. The tool may suggest; a person decides.

## Map

| Where | What |
| --- | --- |
| [protocol/](protocol/) | The FlatBuffers contracts for project RPC (`tsugumi.fbs`), the Agent service (`agent.fbs`), host operations (`host.fbs`) and shared service types (`service_common.fbs`), the native worker (`worker.fbs`) and the remote worker (`remote_worker.fbs`) |
| [core/](core/) | `tsugumid`: the document model, type checking, the scheduler, the embedded Nix evaluator, history and artifacts, versions (git); `core/src/agent/` holds the host-side `tsugumi-agentd` service |
| [sdk/](sdk/) | The Card ABI and runtime, the C++ / Python SDKs, the Nix SDK ([sdk/nix/default.nix](sdk/nix/default.nix)) |
| [macOS/](macOS/) | The SwiftUI app, the shared Linux VM (Virtualization.framework), image definitions |
| [ui/](ui/) | Shared isolated web-module host and UI resources |
| [agent/](agent/) | Shared Agent prompts, contracts ([agent/Shared/](agent/Shared/)) and reference documents; the service implementation is in `core/src/agent/` |
| [web/](web/) | The browser UI and its host server |
| [workerd/](workerd/) | The light native worker, the C++ remote listener/supervisor/Nix runner, and shared worker libraries |
| [cli/](cli/) | The `tsugumi` command (`install.sh` for Mac and Linux, `install.ps1` for Windows, building the distributions) |
| [plugins/](plugins/) | The Claude Code plugin (installed from `.claude-plugin/marketplace.json`; [docs/claude-code.md](docs/claude-code.md)) |
| [nix/](nix/) | Package definitions of `tsugumid`, the worker, the Card SDK and the `tsugumi` command |
| [demos/](demos/) | The bundled demos and their Draft versions (only the shape of the processing, before implementation), as a Japanese and an English set (`demos/ja`, `demos/en`): change both |
| [tests/](tests/) | Tests of the contract, scheduler, history, isolation, demos, fuzzing and more |
| [docs/](docs/) | The list of documents is [docs/README.md](docs/README.md). Astro generates the web version and `llms.txt` in `build/docs/` with `nix run .#build-doc`; only source documents belong in `docs/` |

The overall architecture and the directories in detail are in "The runtime" and "Directories" of [docs/Development.md](docs/Development.md).

## For agents working in this repository

Instructions for Claude Code, Codex and others (`CLAUDE.md` is a link to this file).

### Language

Write documents, code comments, commit messages and pull requests in English. The UI's text is in Japanese and English: the app's English strings are in `macOS/Resources/tools/en_strings.py`.

### Running the Mac app

Someone is doing other work on the same Mac. Do not take the foreground, and do not show dialogs.

- **Launching:** use [macOS/tools/run-ui.sh](macOS/tools/run-ui.sh) (it opens the app in the background with `open -g`, and capturing works as it is). To launch directly, use `open -g -n --env TSUGUMI_UI_SCRIPT=<absolute path> <Tsugumi.app>`; never run the executable directly.
- **Building:** sign with `TSUGUMI_CODESIGN_IDENTITY="Apple Development" macOS/build-app.sh`. An ad hoc signed build is treated as a different app on every rebuild, and reading the API key shows a password dialog ("Signing development builds" in [docs/Development.md](docs/Development.md)). Where there is no certificate (`security find-identity -v -p codesigning`), build as it is.

### Checks before merging

CI runs on GitHub Actions (the Linux and macOS jobs of [.github/workflows/ci.yml](.github/workflows/ci.yml)). To run the same checks locally, use [tests/ci-local.sh](tests/ci-local.sh). On a Mac it runs the macOS job as it is, and the Linux job in a Docker (OrbStack) container.

Run [tests/check-fast.sh](tests/check-fast.sh) before every push: it runs the static tests (`tests/static`, which CI runs first, in about a minute) and builds the Web UI and site player checks.
Use `--no-nix` for only the static tests (seconds).

### Compatibility

- **Do not worry about compatibility until v1.0.0.** Project documents (`tsugumi.json`, `layout.json` and so on), settings, the RPC protocol, storage formats and the contents of the VM and support folders may change without migration code or compatibility layers.
- Do not add code that reads old formats, migrations or deprecated aliases. When something changes, delete the old one.
- The version number is in `VERSION`. This policy holds for the versions before v1.0.0 (0.x).
