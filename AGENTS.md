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
| [protocol/](protocol/) | FlatBuffers contracts: project (`tsugumi.fbs`), Agent (`agent.fbs`), host operations (`host.fbs`, shared types in `service_common.fbs`), workers (`worker.fbs`, `remote_worker.fbs`), data and enums |
| [core/](core/) | Project core `tsugumid`, host services (`src/host/`, `src/agent/`), worker pools, scheduling, Nix, history and versions |
| [client/](client/) | Shared C++ RPC client, C interface and WebAssembly bindings |
| [workerd/](workerd/) | Native Card host, C++ remote listener/supervisor/Nix runner, framing and job support |
| [sdk/](sdk/) | Card ABI and runtime, language SDKs and code generation, views, [Nix SDK](sdk/nix/default.nix) |
| [macOS/](macOS/) | SwiftUI app, RPC/Agent adapters, shared Linux VM, image definitions and app tests |
| [ui/](ui/) | Shared isolated web-module host (`modules/`) and Card scaffolds (`templates/`) |
| [agent/](agent/) | Shared Agent prompts, contracts (`Shared/`) and reference documents |
| [web/](web/) | Browser UI, host server and platform adapters |
| [cli/](cli/) | The `tsugumi` command, installers and distribution packaging |
| [plugins/](plugins/) | Claude Code plugin; [installation and usage](docs/claude-code.md) |
| [nix/](nix/) | Runtime, SDK, remote worker and CLI packages; flake outputs, checks and development shell |
| [demos/](demos/) | Matching Japanese and English demos and Drafts (`ja/`, `en/`): change both |
| [tests/](tests/) | Core, SDK, host, remote worker, static and prompt tests; [commands and manual tools](tests/README.md) |
| [docs/](docs/) | User documents ([index](docs/README.md)), assets and [documentation site tooling](docs/site/README.md) |

The architecture and mechanisms are in [Features](docs/features.md#1-architecture). Release procedures are in [RELEASING.md](RELEASING.md).

## For agents working in this repository

Instructions for Claude Code, Codex and others (`CLAUDE.md` is a link to this file).

### Language

Write documents, code comments, commit messages and pull requests in English. The UI's text is in Japanese and English: the app's English strings are in `macOS/Resources/tools/en_strings.py`.

### Running the Mac app

Someone is doing other work on the same Mac. Do not take the foreground, and do not show dialogs.

- **Launching:** use [macOS/tools/run-ui.sh](macOS/tools/run-ui.sh) (it opens the app in the background with `open -g`, and capturing works as it is). To launch directly, use `open -g -n --env TSUGUMI_UI_SCRIPT=<absolute path> <Tsugumi.app>`; never run the executable directly.
- **Building:** sign with `TSUGUMI_CODESIGN_IDENTITY="Apple Development" macOS/build-app.sh`. An ad hoc signed build is treated as a different app on every rebuild, and reading the API key shows a password dialog ("Signing development builds" in [RELEASING.md](RELEASING.md)). Where there is no certificate (`security find-identity -v -p codesigning`), build as it is.

### Checks before merging

CI runs on GitHub Actions (the Linux and macOS jobs of [.github/workflows/ci.yml](.github/workflows/ci.yml)). To run the same checks locally, use [tests/ci-local.sh](tests/ci-local.sh). On a Mac it runs the macOS job as it is, and the Linux job in a Docker (OrbStack) container.

Run [tests/check-fast.sh](tests/check-fast.sh) before every push: it runs the static tests (`tests/static`, which CI runs first, in about a minute) and builds the Web UI checks, including the demo player.
Use `--no-nix` for only the static tests (seconds). See [tests/README.md](tests/README.md) for suite details and manual tools.

### Compatibility

- **Do not worry about compatibility until v1.0.0.** Project documents (`tsugumi.json`, `layout.json` and so on), settings, the RPC protocol, storage formats and the contents of the VM and support folders may change without migration code or compatibility layers.
- Do not add code that reads old formats, migrations or deprecated aliases. When something changes, delete the old one.
- The version number is in `VERSION`. This policy holds for the versions before v1.0.0 (0.x).
