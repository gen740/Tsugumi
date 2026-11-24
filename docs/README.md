# Tsugumi documents

> **Before v1.0.0, any release may break compatibility.** Project files, the Card ABI, the SDKs and settings can change from one release to the next, without migration. Pin the version you use, and expect to update your projects when you upgrade.

The concept, and a walkthrough with real screens, are in the [README](../README.md).
Start from the document that fits what you want to read. To read with moving figures, there is the [web version](https://gen740.github.io/Tsugumi/) (the same documents). For AI, [llms.txt](https://gen740.github.io/Tsugumi/llms.txt) points to the Markdown of each page.

## Getting started

| Document | Contents |
| --- | --- |
| [features.md](features.md) | Find a feature by task; Card kinds, execution rules and Nix environments |
| [INSTALL.md](INSTALL.md) | Installing and updating the desktop app and the `tsugumi` command (Mac, Linux, Windows), and building from source |
| [Usage.md](Usage.md) | Canvas tools, running from a Card, chart updates, settings and Agent controls |
| [Demos.md](Demos.md) | Choose a demo, try changes and interpret its results |
| [CardGuide.md](CardGuide.md) | Build a temperature conversion Card in C++, change its parameters, and inspect its live view and saved output. Language SDK guides are in [tsugumi-kit](https://github.com/gen740/tsugumi-kit) |
| [RunHistory.md](RunHistory.md) | Inspect and compare runs, check runtime packages and open saved artifacts |
| [RemoteExecution.md](RemoteExecution.md) | Start `tsugumi-workerd` on a remote worker machine and run a Code Card on its Linux host |
| [Visualization.md](Visualization.md) | Matplotlib figure Cards: data parameters, a live view and saved figures |
| [Marimo.md](Marimo.md) | Committed notebook runs, typed inputs/outputs, saved cell results and the interactive-session boundary |
| [Reports.md](Reports.md) | R Markdown and Quarto report Cards, saved HTML and parameterized rendering |
| [WebUI.md](WebUI.md) | The browser workbench and the `tsugumi` command on Mac, Linux and Windows through WSL2 |
| [BinaryCaches.md](BinaryCaches.md) | Attic, Cachix and other signed binary caches, private authentication and VM-wide trust |
| [claude-code.md](claude-code.md) | Having Claude Code make and run Cards through the Claude Code plugin, shown in the Web UI |

## The ideas

| Document | Contents |
| --- | --- |
| [Philosophy.md](Philosophy.md) | "What to build so that we are not used by AI": the concern behind Tsugumi, its name and its author |
| [Rationale.md](Rationale.md) | Why design, implementation and checking share one Workflow: answers to the three reasons for hesitating to hand research to AI (an invisible process, reproducibility, data), the measures that keep trial and error light |
| [Vision.md](Vision.md) | Applications: learning research decisions, growing tools, handing work to the next person, and the idea of business automation |
| [Limitations.md](Limitations.md) | Checks before relying on a result and links to execution constraints |

## How it works

| Document | Contents |
| --- | --- |
| [Versions.md](Versions.md) | Versions: the project's git, each run's version and result (git notes), the version tree and "Start from This Version" |
| [security.md](security.md) | What is and is not protected, the four isolation boundaries (VM, project, Card, RPC), API keys, where data is kept, and known limitations |
| [HostPresentation.md](HostPresentation.md) | Host formatting tools, deterministic editor support and language-dependent version presentation |
| [CoreAndSDK.md](CoreAndSDK.md) | The core knows no language: what the core needs from a language SDK, templates and the Card's views |
| [CardABI.md](CardABI.md) | Card ABI: its fixed schemas, outputs, effects (`send`, cancellation and deadline) and what it leaves out |
| [SandboxContract.md](SandboxContract.md) | What the worker builds for a Card (directories, inputs, outputs, grants, sandbox options, secrets) |
| [Protocol.md](Protocol.md) | The project RPC requests and notifications (generated from `protocol/tsugumi.fbs`) |
| [Glossary.md](Glossary.md) | Card, Draft, Run, invocation, version and other terms |

## Runtime details

| Document | Contents |
| --- | --- |
| [ShortIdentifiers.md](ShortIdentifiers.md) | The identifiers the Agent sees: run numbers, Card ids and short counters instead of UUIDs |
| [VMStoreHealth.md](VMStoreHealth.md) | How the shared VM stops in order, and when its Nix store is checked and repaired |
