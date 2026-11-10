# Plans and guarantees

What Tsugumi can do today, and what is planned. This page gathers what the other documents say.

"What Tsugumi cannot guarantee in principle" lists what no tool can fundamentally solve ([Philosophy](philosophy.md)). "Data and permissions" draws the line between what Tsugumi takes on and what it leaves to a project's author and its users ([the summary in Security](security.md#what-tsugumi-takes-on)). The rest is the current scope and the plans.

## What Tsugumi cannot guarantee in principle

- **A tool cannot guarantee that a result is scientifically right.** People judge that. What Tsugumi provides is a foothold from which the material for that judgement is within reach and other possibilities can be tried at once ([Philosophy](philosophy.md#a-clean-result-alone-cannot-be-checked)).
- **Chance inside the computation cannot be pinned by pinning the environment.** Nix pins the environment and versions pin the code and definitions, so an analysis done once runs the same way on another Mac as long as the project is shared. Deciding conditions such as seeds, for processing whose result depends on random numbers, external data, arrival order or kept state, is up to whoever writes the computation ([Reproducibility](rationale.md#reproducibility)).
- **Going back to an earlier version brings back code and definitions.** Not what was in a worker's memory then, nor data fetched from outside ([What is recorded, and where](rationale.md#what-is-recorded-and-where)).

## Following results

- **A screen that puts two runs side by side is planned in [#156](https://github.com/gen740/Tsugumi/issues/156).** For now, open each run's history and compare.
- **Links from the Agent's report to the Cards and runs it rests on are planned in [#157](https://github.com/gen740/Tsugumi/issues/157).**
- **Keeping each Card's build result (store path) in the run's record is planned in [#158](https://github.com/gen740/Tsugumi/issues/158).** For now, the environment can be traced from the run's commit (which includes `pkgsPath` and `default.nix`).
- **Partial runs use saved inputs.** After changing something upstream, finish by running the whole Workflow ([Rerunning from the middle](rationale.md#rerunning-from-the-middle)).
- **Whether past input and output values are kept is chosen per Card.** Values are kept for Cards with `recordIO` on (off by default; [What is recorded, and where](rationale.md#what-is-recorded-and-where)).

## Data and permissions

- **The Agent is sent only what it read.** Of the sources, logs and input and output values, what the Agent read is sent to the model's provider, and there is no mechanism that masks it automatically. Without the Agent, every operation can be done from the UI ([What is sent to the model](security.md#what-is-sent-to-the-model), [Business automation and the data boundary](vision.md#business-automation-and-the-data-boundary)). Web search is off by default; when on, the search terms also go to the search service ([Web search](security.md#web-search)).
- **Opening a project is a decision to trust its contents.** In someone else's project, `hostFolders` written in a `_card.json` are shared on opening, and `network`, `sandbox: false` and the `nixSources` of `tsugumi.json` take effect at run time, without confirmation. Like running `make` or a notebook, Tsugumi's job is to run it as its author wrote it; whether to trust the author is the user's decision. Tsugumi shows the permissions written on the Card's icons and among the Experimental items, inspects `.tsugumi/` before using it, and asks for approval for saves in which the Agent widens permissions ([What Tsugumi takes on](security.md#what-tsugumi-takes-on), [Known limitations](security.md#10-known-limitations-and-cautions)).
- **Projects are separated the way containers are.** They share one Linux kernel, so a kernel vulnerability is not prevented (the Mac is separated by the VM). The shared `/nix/store` is readable from every project ([Known limitations](security.md#10-known-limitations-and-cautions)).
- **Nix's fixed-output derivations can fetch at build time even with `network: false`.** That is how Nix works; what is fetched is checked against its hash, so it cannot change ([Limits on Nix evaluation](security.md#5-limits-on-nix-evaluation)).

## Where it runs

- **The desktop app runs on Apple silicon with macOS 26 or later.** On Linux and Windows, use the Web UI in the browser (the `tsugumi` command) ([Installation](install.md)).
- **The Mac version installs with the one-line command in the README.** Releases are signed ad hoc, without Apple's notarisation, so a download from a browser is stopped by Gatekeeper. Installing with `curl` is not stopped, and later automatic updates are installed after their checksum and code signature are checked ([Installation](install.md#installing-the-desktop-app-mac)).
- **Linux has been checked on Ubuntu and NixOS.** On aarch64 in an OrbStack VM (Ubuntu 26.04); on x86_64 on a friend's Ubuntu and on a NixOS machine used through the Web UI over SSH ([Over SSH](webui.md#running-it-on-another-machine-over-ssh)). On x86_64, CI (Ubuntu on GitHub Actions) also builds and runs the tests. Linux and Windows need Nix.
- **Windows works through WSL2.** On a friend's Windows, with Nix installed in WSL2 without its daemon, `tsugumi` was installed and a demo opened ([#159](https://github.com/gen740/Tsugumi/issues/159)). Besides `install.ps1`, it can be installed from a shell in the WSL Linux with the same steps as on Linux ([Installation](install.md#using-it-in-the-browser-the-tsugumi-command)).
- **The stable release starts at v1.0.0.** The current 0.x versions are for moving development fast: compatibility of project documents and settings is not kept between versions, and nothing is migrated ([AGENTS.md](https://github.com/gen740/Tsugumi/blob/main/AGENTS.md)).
- **The Claude Code plugin works with the Web UI.** You can hand a Web UI project, opened with the `tsugumi` command or otherwise, to Claude Code ([Using it from Claude Code](claude-code.md#what-is-not-supported)). Claude Code's own shell and file operations do not go through Tsugumi's permission checks.

## Features

- **Cards can be written in C++, Python, bash, R and Julia.** Go, Rust, Ruby and C# are planned in [#10](https://github.com/gen740/Tsugumi/issues/10), [#11](https://github.com/gen740/Tsugumi/issues/11), [#13](https://github.com/gen740/Tsugumi/issues/13) and [#14](https://github.com/gen740/Tsugumi/issues/14). R and Julia Cards run in a separate process and exchange values as JSON (started per invocation; their display appears when the Card ends). Keeping state in a resident worker, and live display, are planned in [#161](https://github.com/gen740/Tsugumi/issues/161).
- **Figures can be drawn by ordinary Python Cards.** Write matplotlib, Plotly or Altair code as it is and it becomes the Card's display. A dedicated Visualization Card that makes figures from templates is planned in [#19](https://github.com/gen740/Tsugumi/issues/19).
- **Dedicated Cards for marimo and R Markdown / Quarto are planned in [#15](https://github.com/gen740/Tsugumi/issues/15) and [#16](https://github.com/gen740/Tsugumi/issues/16).**
- **The logger Card puts the values it receives in the Card's log and display.** Recording to an external database (SQLite, PostgreSQL and the like) is planned in [#160](https://github.com/gen740/Tsugumi/issues/160).
- **The app can have 16 projects open at once.** The number of shared devices of the shared VM decides it (`SharedVM.slotCount`).
- **A large demo (200 Cards) and WASM display are future extensions** ([Future extensions](development.md#future-extensions)).
- **The `load` of the science demos fetches from the publisher on every run.** If the publisher's content changes, so does the result. Cas A is the one pinned by hash ([The bundled demos](demos.md#science-demos-fetching-and-analysing-public-data)).

## What has been checked

- **Tests of the shared VM run on a Mac that can use Virtualization.framework** (`TSUGUMI_TEST_CONTAINER=1`), because GitHub Actions runners cannot run them ([What has been checked](development.md#what-has-been-checked), [Evidence](evidence.md)).
- **Many situations have also been tried with the real Claude API and the ChatGPT plan.** Even so, combinations that were not tried may remain. Automated tests use a test model; one record of going through it with the real API is in [Evidence](evidence.md#a-run-with-the-real-claude-api).
- **Web search is tested automatically with the test model.** It is yet to be checked with the real API and the ChatGPT plan ([Implementation status](development.md#implementation-status)).
