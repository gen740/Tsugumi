# Tsugumi documents

The concept, and a walkthrough with real screens, are in the [README](../README.md).
Start from the document that fits what you want to read. To read with moving figures, there is the [web version](https://gen740.github.io/Tsugumi/) (the same documents). For AI, [llms.txt](https://gen740.github.io/Tsugumi/llms.txt) points to the Markdown of each page.

## Getting started

| Document | Contents |
| --- | --- |
| [INSTALL.md](INSTALL.md) | Installing and updating the desktop app and the `tsugumi` command (Mac, Linux, Windows), and building from source |
| [Usage.md](Usage.md) | Getting started by following a demo's guide, and the operations you use most |
| [CardGuide.md](CardGuide.md) | Your first Card: a Card that takes an input and computes, written in Python, bash, C++, R and Julia |
| [Marimo.md](Marimo.md) | Committed notebook runs, typed inputs/outputs, saved cell results and the interactive-session boundary |
| [Demos.md](Demos.md) | How the bundled demos work: how each demo's Cards connect, what each Card does, and what to try |
| [WebUI.md](WebUI.md) | The same screens in the browser (the `tsugumi` command: the shared VM on a Mac, bubblewrap on Linux, WSL on Windows) |
| [claude-code.md](claude-code.md) | Having Claude Code make and run Cards through the Claude Code plugin, shown in the Web UI |

## The ideas

| Document | Contents |
| --- | --- |
| [Philosophy.md](Philosophy.md) | "What to build so that we are not used by AI": the concern behind Tsugumi, its name and its author |
| [Rationale.md](Rationale.md) | Why design, implementation and checking share one Workflow: answers to the three reasons for hesitating to hand research to AI (an invisible process, reproducibility, data), the measures that keep trial and error light |
| [Vision.md](Vision.md) | Applications: learning research decisions, growing tools, handing work to the next person, and the idea of business automation |
| [Limitations.md](Limitations.md) | The current scope, plans and the scope of the guarantees |

## How it works

| Document | Contents |
| --- | --- |
| [features.md](features.md) | A technical tour of the main features (architecture, Cards and Workflows, Nix, SDK and workers, saving, history, visualisation, UI, Agent, demos) |
| [Versions.md](Versions.md) | Versions: the project's git, each run's version and result (git notes), the version tree and "Start from This Version" |
| [security.md](security.md) | What is and is not protected, the four isolation boundaries (VM, project, Card, RPC), API keys, where data is kept, and known limitations |
| [Protocol.md](Protocol.md) | The RPC contract: the 41 requests and the notifications (generated from `protocol/tsugumi.fbs`) |
| [Glossary.md](Glossary.md) | Card, Draft, Run, invocation, version and other terms |

## Development

| Document | Contents |
| --- | --- |
| [Development.md](Development.md) | Runtime architecture, design decisions, builds and test commands |
| [Lock.md](Lock.md) | Pinned versions of the runtime and its dependencies |
| [LicenseAudit.md](LicenseAudit.md) | License bundling, VM source distribution and dependency updates |
