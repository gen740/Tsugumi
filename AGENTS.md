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
| [protocol/](protocol/) | FlatBuffers contracts: project (`tsugumi.fbs`), Agent (`agent.fbs`), host operations (`host.fbs`, shared types in `service_common.fbs`), private VM helper (`vm_host.fbs`), workers (`worker.fbs`, `remote_worker.fbs`), data and enums. The Card ABI is core-owned here: [`card.fbs`](protocol/card.fbs) and [`card_abi.h`](protocol/include/tsugumi/card_abi.h) ([Card ABI](docs/design/CardABI.md)) |
| [util/](util/), [rpc/](rpc/) | Generic filesystem/process/JSON mechanisms and protocol-free transport/TLS |
| [tsugumid/](tsugumid/) | Project document contracts, runtime, scheduling, history and versions; narrow shared runtime accounting |
| [hostd/](hostd/) | Human/Agent policy, settings, credentials, document operations and environment adapters |
| [hostd/cache/](hostd/cache/) | `tsugumi-cached`, the own Nix binary cache server ([design](docs/design/BinaryCacheServer.md)); its own process because it links Nix |
| [client/](client/) | Shared C++ RPC client, C interface and WebAssembly bindings |
| [workerd/](workerd/) | Native Card host `tsugumi-worker`, remote listener/supervisor/Nix runner `tsugumi-workerd`, framing and job support ([worker implementation](workerd/README.md)) |
| [vm/](vm/) | Release-built Linux guest and independent macOS VM runtime/helper ([lifecycle](docs/design/VMStoreHealth.md)) |
| [ui/macos/](ui/macos/) | SwiftUI app and Host/Agent presentation adapters |
| [ui/shared/](ui/shared/) | Shared isolated host for Card views and the UI strings |
| [hostd/agent/](hostd/agent/) | Agent implementation, prompts, tool descriptions and guide scripts |
| [ui/web/](ui/web/) | Browser UI, host server and platform adapters |
| [cli/](cli/) | The `tsugumi` command, installers and distribution packaging |
| [pack/](pack/) | `tsugumi-pack`: exports a Project as a standalone Mac app ([design](pack/README.md)) |
| [plugins/](plugins/) | Claude Code plugin; [installation and usage](docs/claude-code.md) |
| [nix/](nix/) | Runtime, remote worker and CLI packages, Card evaluation entry; flake outputs, checks and development shell. The language SDKs and example templates live in [tsugumi-kit](https://github.com/gen740/tsugumi-kit), pinned in `flake.lock`; templates are copied from the Project (`templates/`) and Host stores ([Templates](docs/design/CoreAndSDK.md#templates)) |
| [tsugumi-demos](https://github.com/gen740/tsugumi-demos) | Japanese and English demos and Drafts (`ja/`, `en/`), pinned in `flake.lock`. Commit both languages there, then run `nix flake update tsugumi-demos` here. |
| [tests/](tests/) | Core, host, remote worker, static and prompt tests; [commands and manual tools](tests/README.md). SDK tests live in tsugumi-kit |
| [docs/](docs/) | [User guides](docs/README.md), [design documents](docs/design/README.md), assets and [documentation site tooling](docs/site/README.md) |

Start with the [design index](docs/design/README.md) or [architecture](docs/design/Architecture.md). Release procedures are in [RELEASING.md](RELEASING.md).

## For agents working in this repository

Instructions for Claude Code, Codex and others (`CLAUDE.md` is a link to this file).

### Language

Write documents, code comments, commit messages and pull requests in English. The UI's text is in Japanese and English: the English strings are in `ui/shared/i18n/en.json`.

### Running the Mac app

Someone is doing other work on the same Mac. Do not take the foreground, and do not show dialogs.

- **Launching:** use [ui/macos/tools/run-ui.sh](ui/macos/tools/run-ui.sh) (it opens the app in the background with `open -g`, and capturing works as it is). To launch directly, use `open -g -n --env TSUGUMI_UI_SCRIPT=<absolute path> <Tsugumi.app>`; never run the executable directly.
- **Building:** sign with `TSUGUMI_CODESIGN_IDENTITY="Apple Development" ui/macos/build-app.sh`. An ad hoc signed build is treated as a different app on every rebuild, and reading the API key shows a password dialog ("Signing development builds" in [RELEASING.md](RELEASING.md)). Where there is no certificate (`security find-identity -v -p codesigning`), build as it is.

### Where commands live

- What is built from a definition (programs, the guest, packages) is built by CMake or Nix.
- Commands for working on the repository are flake apps: `nix run .#guest-image` (the guest image from its CI artifact; `--build` builds it here), `nix run .#release`, and the documentation's `.#dev-doc` / `.#build-doc` / `.#test-doc`. CI steps call the same commands, so `tests/ci-local.sh` can run them too.
- Shell scripts remain only where the host is needed: Apple's toolchain, `codesign`, `xcrun` (`ui/macos/build-app.sh`, `hostd/tools/build-macos.sh`, `vm/macos/tools/build-vmctl.sh`, `cli/package-*.sh`, `ui/macos/tools/run-ui.sh`).

### Branches

Development happens on `dev`, without pull requests: commit small changes and fixes directly onto `dev` (rebase onto `origin/dev` and push as a fast-forward). `dev`'s work goes back to `main` through a pull request from `dev` to `main`; `main` changes in no other way. CI runs on every push to `dev` and on the pull request to `main`.

### Checks before merging

CI runs on GitHub Actions ([.github/workflows/ci.yml](.github/workflows/ci.yml) calls [.github/workflows/ci-platform.yml](.github/workflows/ci-platform.yml)). To run the same checks locally, use [tests/ci-local.sh](tests/ci-local.sh). On a Mac it runs the macOS job as it is, and the Linux job in a Docker (OrbStack) container.

Run [tests/check-fast.sh](tests/check-fast.sh) before every push: it runs the static tests (`tests/static`, which CI runs first, in about a minute) and builds the Web UI checks, including the demo player.
Use `--no-nix` for only the static tests (seconds). See [tests/README.md](tests/README.md) for suite details and manual tools.

### Compatibility

- **Do not worry about compatibility until v1.0.0.** Project documents (`tsugumi.json`, `layout.bin` and so on), settings, the RPC protocol, storage formats and the contents of the VM and support folders may change without migration code or compatibility layers.
- Do not add code that reads old formats, migrations or deprecated aliases. When something changes, delete the old one.
- The version number is in `VERSION`. This policy holds for the versions before v1.0.0 (0.x).
