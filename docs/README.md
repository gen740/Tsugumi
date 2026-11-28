# Tsugumi user guides

Start with the [README](../README.md) for a walkthrough or use the
[documentation site](https://gen740.github.io/Tsugumi/) to explore the demos.
The site also publishes a Markdown catalog at
[llms.txt](https://gen740.github.io/Tsugumi/llms.txt).

Project files, settings, the Card ABI and the SDKs can change between
pre-1.0 releases. Use matching versions of Tsugumi and tsugumi-kit.

## Using the workbench

| Document | Contents |
| --- | --- |
| [Feature finder](features.md) | Find a workbench feature by task |
| [Installation](INSTALL.md) | Install, update and build Tsugumi |
| [Usage](Usage.md) | Canvas tools, runs, settings and Agent controls |
| [Bundled demos](Demos.md) | Choose a demo, try a change and inspect the result |
| [Run history](RunHistory.md) | Inspect and compare runs, look up runtime packages and open artifacts |
| [Versions](Versions.md) | Browse Git versions and start a new trial from a saved version |
| [Before relying on a result](Limitations.md) | Check the conditions, evidence and permissions behind a result |
| [Glossary](Glossary.md) | Cards, Drafts, runs, invocations and other terms |

## Writing Cards

| Document | Contents |
| --- | --- |
| [Your first Card](CardGuide.md) | Build a C++ Card, change its parameters and inspect its outputs |
| [Project files](Reference.md) | Project manifests, Card definitions, IDs, child projects and Nix exports |
| [Visualization](Visualization.md) | Matplotlib figures, display messages and saved results |
| [marimo notebooks](Marimo.md) | Committed cells, typed inputs and outputs, and saved results |
| [R reports](Reports.md) | Render R Markdown and Quarto reports with parameters |

Language SDK guides and templates live in [tsugumi-kit](https://github.com/gen740/tsugumi-kit).

## Connecting tools and services

| Document | Contents |
| --- | --- |
| [Web UI](WebUI.md) | Open the workbench in a browser with the `tsugumi` command |
| [Remote execution](RemoteExecution.md) | Register a Linux worker and inspect or remove retained job data |
| [Binary caches](BinaryCaches.md) | Configure signed Nix substituters |
| [Claude Code](claude-code.md) | Make and run Cards through the plugin |
| [Codex](codex.md) | Make and run Cards from Codex through the Mac app's `tsugumi` command |

## The ideas

| Document | Contents |
| --- | --- |
| [Philosophy](Philosophy.md) | Human judgment, reproducible trials and the meaning of Tsugumi's name |
| [Vision](Vision.md) | Teaching, tool building and handing over a Workflow |

For implementation, start at the [design index](design/README.md). It covers
architecture, execution, storage, [security](design/security.md) and the
[reason for sharing one Workflow](design/Rationale.md).
