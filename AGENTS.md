# AGENTS.md

## The project's philosophy

The full text is in [docs/Philosophy.md](docs/Philosophy.md). In short:

- People ask the questions, give the results meaning and take responsibility. AI moves the trial and error forward (AI-driven science).
- The hard part is whether people can come back to judging after handing work over. As with Level 3 of automated driving, "a person checks at the end" means nothing if the process has been lost from sight.
- So keep the process, not a clean result and a long report. Split processing into pieces you can see through (Cards), make it possible to follow what went in, what was done and what came out, and let people change the conditions of a step they are curious about and run it again by their own hand.
- This project is itself an experiment, built during the one week of a Claude hackathon in a back-and-forth where a person wrote intentions and specifications and AI gave them form. What was prepared beforehand and what was made in that week is in [How it was built](docs/Philosophy.md#how-it-was-built); the state at submission is the archived [gen740/tsugumi-hackathon](https://github.com/gen740/tsugumi-hackathon).

## Map

| Where | What |
| --- | --- |
| [protocol/](protocol/) | The FlatBuffers contract. The UI, the Web and the Agent all talk to `tsugumid` through this same RPC |
| [core/](core/) | `tsugumid`: the document model, type checking, the scheduler, the embedded Nix evaluator, history and artifacts, versions (git) |
| [sdk/](sdk/) | The Card ABI and runtime, the C++ / Python SDKs, the Nix SDK ([sdk/nix/default.nix](sdk/nix/default.nix)), the worker |
| [macOS/](macOS/) | The SwiftUI app, the shared Linux VM (Virtualization.framework), image definitions |
| [ui/](ui/) | Dawn's rendering layer, the viewer for charts (Plotly, Vega-Lite) |
| [agent/](agent/) | The Agent (model connections, conversations, tools derived from the RPC). The contract shared with the Web is in [agent/Shared/](agent/Shared/) |
| [web/](web/) | The browser UI and its host server |
| [cli/](cli/) | The `tsugumi` command (`install.sh` for Mac and Linux, `install.ps1` for Windows, building the distributions) |
| [plugins/](plugins/) | The Claude Code plugin (installed from `.claude-plugin/marketplace.json`; [docs/claude-code.md](docs/claude-code.md)) |
| [nix/](nix/) | Package definitions of `tsugumid`, the worker, the Card SDK and the `tsugumi` command |
| [demos/](demos/) | The bundled demos, and their Draft versions (only the shape of the processing, before implementation) |
| [tests/](tests/) | Tests of the contract, scheduler, history, isolation, demos, fuzzing and more |
| [docs/](docs/) | The list of documents is [docs/README.md](docs/README.md). The web version and [docs/llms.txt](docs/llms.txt) for AI are generated with `node docs/site/build.mjs` |

The overall architecture and the directories in detail are in "The runtime" and "Directories" of [docs/Development.md](docs/Development.md).

## Claims, and where to check them

| Claim | Where to check |
| --- | --- |
| Each Card's environment is pinned with Nix, and build results are reused | [sdk/nix/default.nix](sdk/nix/default.nix), [docs/features.md](docs/features.md) §3, [tests/cpp](tests/cpp) (Nix upstream references) |
| The types of ports check that wiring is consistent | "Typed ports" in [docs/features.md](docs/features.md) §2, [tests/cpp](tests/cpp) (types, structural compatibility, merge unions) |
| Branches, merges, bounded loops and Cards in Cards run | [tests/python/test_scheduler.py](tests/python/test_scheduler.py) (loops: Collatz in [tests/python/test_demos.py](tests/python/test_demos.py)) |
| What is shown as finished has a saved record behind it | [docs/Rationale.md](docs/Rationale.md), [tests/python/test_regressions.py](tests/python/test_regressions.py) (a run whose history failed to save does not succeed), [tests/python/test_contract.py](tests/python/test_contract.py) (fault injection) |
| Each run's version and result can be followed, and work can restart from an old version | [docs/Versions.md](docs/Versions.md), [tests/python/test_versions.py](tests/python/test_versions.py), [tests/python/test_history.py](tests/python/test_history.py) |
| Partial runs start from the middle (with saved inputs) | "Partial runs" in [docs/Rationale.md](docs/Rationale.md), [tests/python/test_regressions.py](tests/python/test_regressions.py) |
| Cards are isolated, and their network is off by default (Nix's fixed-output derivations can communicate at build time: [docs/security.md](docs/security.md) §5) | [docs/security.md](docs/security.md), [tests/python/test_host_isolation.py](tests/python/test_host_isolation.py) (CI on Linux and macOS; macOS is skipped for documentation-only changes), [tests/python/test_host_sandbox.py](tests/python/test_host_sandbox.py), and in the VM [tests/python/test_cards_container.py](tests/python/test_cards_container.py) (`TSUGUMI_TEST_CONTAINER=1`, on a local Mac) |
| Reading the `.tsugumi/` of someone else's project cannot rewrite anything outside it | [tests/python/test_untrusted_project.py](tests/python/test_untrusted_project.py), "Data and permissions" in [docs/Limitations.md](docs/Limitations.md) |
| A save in which an Agent (the chat in the app or the Web, or the Claude Code plugin) widens a Card's permissions with Tsugumi's tools does not go through without a person's approval (Claude Code's own shell and file operations are outside this) | [tests/python/test_permissions.py](tests/python/test_permissions.py), [tests/python/test_claude_code_plugin.py](tests/python/test_claude_code_plugin.py) (the plugin's key cannot answer approvals), "The Agent's permission boundary" in [docs/security.md](docs/security.md) §8 |
| Both people and the Agent can write comments on a Card, and who did what is recorded (for the Agent, not on the model's own word). How each version changed them can be followed | "Generating Cards from comments" in [docs/features.md](docs/features.md), [tests/python/test_card_comments.py](tests/python/test_card_comments.py) |
| The UI and the Agent are equal clients of the same contract | [protocol/tsugumi.fbs](protocol/tsugumi.fbs), [tests/python/test_agent_contract.py](tests/python/test_agent_contract.py) |
| Claude's features (models, Skills, subagents, MCP, images, web search) are each used for what they are good at | "Choosing Claude's features by purpose" in [docs/features.md](docs/features.md) §9, `ToolImageTests` (images) and `TeamTests` (helpers) in `macOS/CoreTests`, [tests/python/test_claude_code_plugin.py](tests/python/test_claude_code_plugin.py) (MCP) |
| With the real Claude API, Cards can be made from Drafts, run and checked | "A run with the real Claude API" in [docs/Evidence.md](docs/Evidence.md) (including the attempts that did not work, and what was fixed after them) |
| Resident workers and the like keep trial and error light | "Measures that keep trial and error light" in [docs/Rationale.md](docs/Rationale.md), the measurements in [docs/Development.md](docs/Development.md) |
| It runs on macOS, Linux and Windows (checked on macOS and on Linux, Ubuntu and NixOS; on Windows through WSL2, as far as opening a demo) | "Using it in the browser" in [INSTALL.md](INSTALL.md), "Where it runs" in [docs/Limitations.md](docs/Limitations.md), [tests/python/test_cli.py](tests/python/test_cli.py) (the installers and the WSL handling, not real machines) |
| Cards can be made and run as the documentation says, and AI can be given the same documents. The documents' links, paths and lists match what exists | [tests/python/test_docs.py](tests/python/test_docs.py) (runs the code in [docs/CardGuide.md](docs/CardGuide.md), and checks the links and paths in [docs/llms.txt](docs/llms.txt) and every page) |
| What was prepared before the hackathon and what was made during it | [How it was built](docs/Philosophy.md#how-it-was-built), the archived [gen740/tsugumi-hackathon](https://github.com/gen740/tsugumi-hackathon), the Git history |

A table with the commands and the last environment checked is in [docs/Evidence.md](docs/Evidence.md); the list of tests is in "Tests" and "What has been checked" of [docs/Development.md](docs/Development.md); CI is in [.github/workflows/ci.yml](.github/workflows/ci.yml).

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

### Compatibility

- **Do not worry about compatibility until v1.0.0.** Project documents (`tsugumi.json`, `layout.json` and so on), settings, the RPC protocol, storage formats and the contents of the VM and support folders may change without migration code or compatibility layers.
- Do not add code that reads old formats, migrations or deprecated aliases. When something changes, delete the old one.
- The version number is in `VERSION`. This policy holds for the versions before v1.0.0 (0.x).
