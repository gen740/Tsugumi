# Tsugumi documents

The concept, and a walkthrough with real screens, are in the [README](../index.md).
Start from the document that fits what you want to read. To read with moving figures, there is the [web version](https://gen740.github.io/Tsugumi/) (the same documents). For AI, [llms.txt](../llms.txt) points to the Markdown of each page.

## Getting started

| Document | Contents |
| --- | --- |
| [INSTALL.md](install.md) | Installing and updating the desktop app and the `tsugumi` command (Mac, Linux, Windows), and building from source |
| [Usage.md](usage.md) | Getting started by following a demo's guide, and the operations you use most |
| [CardGuide.md](cardguide.md) | Your first Card: a Card that takes an input and computes, written in Python, bash, C++, R and Julia (a test runs the code on this page) |
| [Demos.md](demos.md) | How the bundled demos work: how each demo's Cards connect, what each Card does, and what to try |
| [WebUI.md](webui.md) | The same screens in the browser (the `tsugumi` command: the shared VM on a Mac, bubblewrap on Linux, WSL on Windows) |
| [claude-code.md](claude-code.md) | Having Claude Code make and run Cards through the Claude Code plugin, shown in the Web UI |

## The ideas

| Document | Contents |
| --- | --- |
| [Philosophy.md](philosophy.md) | "What to build so that we are not used by AI": the concern behind Tsugumi, where it started, its name, how it was built and its author |
| [Rationale.md](rationale.md) | Why design, implementation and checking share one Workflow: answers to the three reasons for hesitating to hand research to AI (an invisible process, reproducibility, data), the measures that keep trial and error light, and where to check each |
| [Vision.md](vision.md) | Applications: learning research decisions, growing tools, handing work to the next person, and the idea of business automation |
| [Limitations.md](limitations.md) | The current scope, plans and the scope of the guarantees |

## How it works

| Document | Contents |
| --- | --- |
| [features.md](features.md) | A technical tour of the main features (architecture, Cards and Workflows, Nix, SDK and workers, saving, history, visualisation, UI, Agent, demos) |
| [Versions.md](versions.md) | Versions: the project's git, each run's version and result (git notes), the version tree and "Start from This Version" |
| [security.md](security.md) | What is and is not protected, the four isolation boundaries (VM, project, Card, RPC), API keys, where data is kept, and known limitations |
| [Protocol.md](protocol.md) | The RPC contract: the 41 requests and the notifications (generated from `protocol/tsugumi.fbs`) |
| [Glossary.md](glossary.md) | Card, Draft, Run, invocation, version and other terms |

## Records and development

| Document | Contents |
| --- | --- |
| [Evidence.md](evidence.md) | How to check each claim: test names, commands and the last environment checked, and a run with the real Claude API |
| [Development.md](development.md) | Design decisions of the runtime, tests and what has been checked, measurements, implementation status |
| [Lock.md](lock.md) | Pinned versions of the runtime and its dependencies |
| [LicenseAudit.md](licenseaudit.md) | The license audit of Tree-sitter, fonts, the VM and more, and what remains for distribution |
