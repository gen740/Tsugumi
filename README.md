<p align="center">
  <a href="https://gen740.github.io/Tsugumi/"><img src="docs/assets/tsugumi-hero.svg" alt="Tsugumi: open the documentation" width="100%"></a>
</p>

<p align="center">
  <a href="https://gen740.github.io/Tsugumi/"><img src="docs/assets/docs-card.svg" alt="Read the documentation: the problem told with moving figures, demos you can watch without installing, and how it works" width="100%"></a>
</p>

<p align="center">
  <a href="#install"><img src="docs/assets/install-card.svg" alt="Install it and try it: the app on a Mac, a command for the browser on Linux and Windows. Go to installation and first steps" width="100%"></a>
</p>

<p align="center">
  <a href="https://gen740.github.io/Tsugumi/"><img src="https://img.shields.io/badge/docs-gen740.github.io%2FTsugumi-c97d3d" alt="Documentation on the web"></a>
  <a href="https://github.com/gen740/Tsugumi/actions/workflows/ci.yml"><img src="https://github.com/gen740/Tsugumi/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI"></a>
  <a href="https://github.com/gen740/Tsugumi/releases/latest"><img src="https://img.shields.io/github/v/release/gen740/Tsugumi?label=release" alt="Latest release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue" alt="MIT License"></a>
  <a href="INSTALL.md"><img src="https://img.shields.io/badge/platform-macOS_%7C_Linux_%7C_Windows-lightgrey" alt="macOS, Linux and Windows (Windows through WSL2)"></a>
  <a href="docs/Philosophy.md#how-it-was-built"><img src="https://img.shields.io/badge/built_with-Claude_Code-D97757" alt="Built with Claude Code"></a>
  <a href="docs/llms.txt"><img src="https://img.shields.io/badge/docs-for_humans_%26_AI-2d5a5c" alt="Documentation written for both people and AI"></a>
</p>

# Tsugumi

**Run the analysis you sketched with AI, as it is, and check it with your own hands.**

Tsugumi is a research workbench in which the specification a person draws, the thing an AI edits and the processing that actually runs are one and the same Workflow. It runs on macOS, Linux and Windows (Windows through WSL2).
You draw a computation or a data pipeline as Cards and the wires between them, implement it together with an AI, and run it in environments pinned with Nix.
You open the code or the result of a Card you are curious about, change a condition and try again with the inputs that were saved. Going back and forth like this, a researcher comes to understand how the analysis got where it did, and can ask the next question.

![A Canvas that makes an RGB image and an energy spectrum from an X-ray observation of Cas A](docs/assets/screenshots/workflow.png)

<p align="center">
  <a href="https://www.youtube.com/watch?v=-GhD9vI6_Dg"><b>Demo video</b></a> ·
  <a href="#are-you-using-ai-in-your-research-as-much-as-you-could">The problem</a> ·
  <a href="#following-an-experiment-on-screen">On screen</a> ·
  <a href="#install"><b>Install</b></a> ·
  <a href="#first-steps">First steps</a> ·
  <a href="#bundled-demos">Demos</a> ·
  <a href="#documentation">Documentation</a>
  |
  <a href="docs/Philosophy.md">Why it was made</a> ·
  <a href="docs/Philosophy.md#the-name">The name</a> ·
  <a href="https://gen740.github.io/Tsugumi/">Read it with moving figures</a>
</p>

<p align="center">
  <a href="https://www.youtube.com/watch?v=-GhD9vI6_Dg"><img src="https://i.ytimg.com/vi/-GhD9vI6_Dg/hqdefault.jpg" alt="Demo video: Tsugumi, run the analysis you sketched with AI and check it with your own hands" width="560"></a><br>
  <a href="https://www.youtube.com/watch?v=-GhD9vI6_Dg">▶ Watch the demo video (YouTube, in Japanese)</a>
</p>

- **The diagram you draw is what runs.** Wire up Drafts and the Agent writes the code, the types and the Nix environment; that Workflow is what gets executed.
- **Try just the Card you are curious about, right away.** Run again only the Cards or child Workflows you choose, with the inputs that were saved.
- **Pinned down to the environment, so it can be checked again.** Each Card's dependencies are pinned with Nix, and every run is recorded against a git version.
- **Computation runs isolated.** In a VM of its own on a Mac, under bubblewrap on Linux. Cards have no outside network by default, and a save in which the Agent widens a Card's permissions does not go through without the user's approval. What is sent to the model is [stated](docs/security.md#what-is-sent-to-the-model). Opening a project is a decision to trust its contents; Tsugumi takes responsibility for what happens inside that ([what it guarantees](docs/security.md#what-tsugumi-takes-on)).
- **A finished Workflow needs no LLM.** The generated code stays as ordinary files that a person can read and fix at any time.
- **Each Claude feature for what it is good at.** Tool use to implement Cards, subagents to implement independent Drafts at the same time, a Skill for the demo guide, multimodal input for the figures a run makes, and MCP (the Claude Code plugin) for agents outside. Small models write the guide and the summaries ([the list](docs/features.md#choosing-claudes-features-by-purpose)).

## Are you using AI in your research as much as you could?

AI has begun to take on long-standing problems in mathematics and science.
Between August and September 2026, OpenAI announced [solutions to, or major progress on, ten open problems](https://openai.com/index/ten-advances-in-mathematics/), and Anthropic's Claude announced [an improved lower bound on the Riemann zeta function](https://www.anthropic.com/research/riemann-zeta) and [a complete formalisation of Fermat's Last Theorem](https://www.anthropic.com/research/formalizing-fermats-last-theorem). Each formalised its argument in a proof assistant such as Lean and published proofs a computer can [check mechanically](https://lean-lang.org/doc/reference/latest/ValidatingProofs/).
In October, the physicist Matthew Schwartz and Anthropic ran Claude inside a computational harness that could be checked rigorously, and in a few weeks it computed 15 Feynman integrals of particle physics that had never been computed before ([Claude-shaped science](https://www.anthropic.com/news/claude-shaped-science)).

People can weigh these results, understand what they mean and ask the next question because the AI's arguments and computations can be checked by an independent mechanism.

**To hand work to an AI, you need a way to check that work.**

Why, then, do we hesitate to hand more of our everyday research to AI?

1. **Reproducibility is not assured.** The code, the inputs and the environment behind a result are not kept together, so the same computation cannot be checked again.
2. **The path to the conclusion is invisible.** You cannot follow which data was processed how, or where which decision was made.
3. **Handing data to an AI is frightening.** You cannot tell where unpublished data is sent or how it is handled.

What these share is that **there is no foothold from which a person can follow, afterwards, what the AI did and what it handled.**

## Every check raises the next question

With AI, the time from writing code to running experiments to summarising results has shrunk dramatically.
What was tried and what was learned arrives at once, as a tidy summary or report.
But the faster and larger the trial and error becomes, the harder it is to hold a report written in words up against the processing that actually happened.

"What would change if I altered just this condition?"
"I'd like to try a different approach just here."

Each time, you ask for another attempt and read through the long report that comes back.
You search thousands of lines of code and explanation for an assumption that quietly changed, and recheck what the conclusion rests on.
The experiments themselves run fast, but you come to a halt at the stage where a person has to digest the process and turn it into the next idea.

**Weighing results, deciding what to accept as evidence and choosing what to ask next is the researcher's own work.**
Checking is also a creative place where new ideas are born.
Would the same trend appear with a different way of estimating the background? Would another model explain the difference that could not be explained?
Each time you change a condition and compare results, you see how far a hypothesis holds, and something new to look into appears.

In Tsugumi you can put your hands directly on the place where the question arose.
Read a Card's code, change a parameter and try it with the saved inputs. Go back to an earlier approach in the version history and start a different one.
Tune things with partial runs, and finally run the whole Workflow to confirm the result.

**From understanding how an analysis went to trying your own question: I want to make that distance short.**
By keeping the trial and error done with AI in a structure you can see across, Tsugumi gives researchers time to think hard about hypotheses and results, and room to move on to the next attempt.

## What we want to ask of AI should be simple

Download the observation, split it by energy, plot it.

What you want is simple enough to get across by drawing three boxes on paper.
Yet today we write long instructions, read long reports, and check again whether we were understood.

## Draw the shape of the processing. Implement it with AI. Run it and check.

Tsugumi is a **macOS app for assembling computation and data processing as a graph of Cards (a Workflow)**.

1. **Draw.** Place Drafts, which have only a name and a description, and wire them up to design the whole flow.
2. **Implement.** Leave a Draft to the Agent, or write C++ / Python / bash / R / Julia yourself. The generated code stays in the project as ordinary files that you can reread and edit at any time.
3. **Check.** Run the graph in environments pinned with Nix, and look at each Card's progress, outputs and logs. When you fix a step in the middle, run again from there at once, with the saved inputs.

## Splitting the processing finely, without making trial and error heavy

The more finely you split processing into pieces you can work on, the more effort goes into types and environments, and the more time goes into starting and recording. Tsugumi takes this on with the Agent and its runtime. From a Draft, the Agent writes the code, the output types and the Nix environment; changing a condition does not trigger a rebuild; a partial run reruns only the Cards you chose, with saved inputs; and a run is reported as finished only after its record is saved.

**The structure a person draws, the structure the Agent implements and the structure that runs are the same.**
The generated code stays as ordinary files, and a finished Workflow runs without an LLM. Why these measures were taken, and where their code and tests are, is in [Measures that keep trial and error light](docs/Rationale.md#measures-that-keep-trial-and-error-light).

## Following an experiment on screen

The bundled "Cas A · Chandra RGB" demo fetches an X-ray observation (Chandra) of the supernova remnant Cassiopeia A, pinned with Nix, and splits its photons into three energy bands to make an RGB image.
It also draws the energy spectrum of the same photons with matplotlib, so you can see which element's emission lines became which colour.
Switch to the deep observation ObsID 114 (50 ks, about 15 million photons in the three bands) and the lines in the spectrum stand out more clearly.

### Seeing the whole flow

In the screen at the top of this page, a sticky note states the aim of the analysis, and below it runs the flow `load` → `bands` → `rgb` / `spectrum`.
When it runs, each Card shows its progress, and the table of photon counts per band, the RGB image and the spectrum appear right on the Canvas.
The spectrum shows that the green band (1.65–2.25 keV) contains exactly the Si line, while S, Ar, Ca and Fe fall in blue. What the colours of the image mean can be checked on the same screen.
From the chat on the right you can ask the Agent to implement or change things.

### Reading the code and trying other conditions

Open a Card's settings to see its source code, its input and output ports, and the definition of its environment (`default.nix`).
`spectrum` is the same matplotlib code you would write for a window: the figure drawn with `plt.show()` appears as the Card's display.
Rewrite the band edges or the bin width of the spectrum and rerun only that Card with the saved inputs, and checking continues from the very place you were curious about.

<a href="https://gen740.github.io/Tsugumi/#demo-casa"><img src="docs/assets/casa-source.svg" alt="Opening the settings of the spectrum Card and following its matplotlib source down to plt.show()" width="100%"></a>

### Looking back at what actually ran

The timeline at the bottom of the screen shows when each Card ran and for how long.
The observation and the analysis tools are fetched and built once, on the first run; after that they come from the Nix store, so the whole Run recorded here finishes in about 12 seconds.
You can also open each Card's run history, logs and artifacts, and check every step on the way to the conclusion.

<a href="https://gen740.github.io/Tsugumi/#demo-casa"><img src="docs/assets/casa-timeline.svg" alt="The timeline drawing, as the run goes, when each Card ran and for how long" width="100%"></a>

### Going inside a step

A large step can be gathered into a child Workflow (a Card in a Card). Open "collatz-cpp" in the "Collatz (C++ / Python / Julia / R)" demo and the Workflow inside it appears, with a branch and a loop.
Most of the time you handle it as a single Card; when you want to check, you can go down to its finest steps.

<a href="https://gen740.github.io/Tsugumi/#demo-collatz"><img src="docs/assets/collatz-subworkflow.svg" alt="The Workflow inside collatz-cpp, going round its branch and loop 112 times" width="100%"></a>

### Letting Tsugumi the thrush show you around

The first time you open the app, the thrush in the bottom right asks "What would you like to do?". Choose between watching a demo run and having the AI implement Drafts, pick a field, and it takes you to a demo.
In a project, the chat is the thrush. Write `$demo` and it flies around the screen, pointing at Cards, the Run button and a Card's settings, and explains one step at a time with choices to pick from (with an OpenAI or Anthropic API key, or a ChatGPT plan; without a key, three demos have a fixed script).

<a href="https://gen740.github.io/Tsugumi/#demo-casa"><img src="docs/assets/casa-guide.svg" alt="The thrush flying to the spectrum Card and explaining it in a speech bubble with choices" width="100%"></a>

## Trying it

### Install

**We recommend the macOS desktop app.** Tsugumi is developed on macOS, so this is the most thoroughly checked way to use it. The runtime for computation is inside the app, and there is nothing else to prepare.

**Mac (Apple silicon, macOS 26 or later)**: the desktop app. It contains the runtime and the demos, and updates itself from then on.

```bash
curl -fL -o /tmp/Tsugumi.zip https://github.com/gen740/Tsugumi/releases/latest/download/Tsugumi.zip && ditto -x -k /tmp/Tsugumi.zip /Applications && open /Applications/Tsugumi.app
```

**Mac and Linux**: the `tsugumi` command, used in the browser (on Linux, install [Nix](https://nixos.org/download) first). Open it with `tsugumi` and update it with `tsugumi update`.

```bash
curl -fsSL https://github.com/gen740/Tsugumi/releases/latest/download/install.sh | sh
```

**Windows** (WSL2): run this in PowerShell (no administrator rights needed). First install WSL2 (`wsl --install` in PowerShell) and [Nix](https://nixos.org/download) in its Linux.

```powershell
irm https://github.com/gen740/Tsugumi/releases/latest/download/install.ps1 | iex
```

You can also open a shell in the WSL Linux and install with the same `curl … | sh` as on Mac and Linux. Either way, running `tsugumi` opens the page in the Windows browser.

The first start takes a few minutes to prepare. The signature (ad hoc), system requirements and building from source are in [INSTALL.md](INSTALL.md).

### First steps

1. **Open a demo.** From "Browse Demos" on the home screen, choose "Open a Copy" on the demo you want. An editable copy is made for you; the bundled original is left unchanged.
2. **Run it.** Once the first preparation is done, press "Run" at the top of the screen. Open a Card, change its code or its conditions, and rerun just that Card.
3. **Let the thrush guide you.** Send `$demo` in the chat and the thrush points at the screen and guides you step by step. Without an API key, three demos (Data to report, Kepler-6 b and Pharmacokinetics) have a fixed script.
4. **Connect the Agent (optional).** Register an Anthropic or OpenAI API key under "API" in the settings, or sign in with "Continue with ChatGPT", and the Agent can make Cards, and the thrush answers questions in any demo. The Agent is sent the conversation and what its tools read, such as sources and logs ([what is sent to the model](docs/security.md#what-is-sent-to-the-model)). Without the Agent, everything (making, editing and running Cards) can still be done on screen.

The app and the Web UI are in Japanese by default; choose English under "General" in the settings.

If you use Claude Code, the `tsugumi` plugin lets Claude Code work on a project in the Web UI ([Using it from Claude Code](docs/claude-code.md)).

### Bundled demos

Each one replays the record of a real run. Click one to open it in the [web version](https://gen740.github.io/Tsugumi/#demos), where you can read what is inside its Cards and their sources. There are 31 demos, Draft versions included; how each works is in [The bundled demos](docs/Demos.md).

#### The basics

<table>
<tr><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-report"><img src="docs/assets/demos/report.svg" alt="The Data to report demo running" width="100%"></a><br><b>Data to report</b><br><sub>Generate data, aggregate it and write a report. Change a step in the middle and rerun from there.</sub></td><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-collatz"><img src="docs/assets/demos/collatz.svg" alt="The Collatz (C++ / Python / Julia / R) demo running" width="100%"></a><br><b>Collatz (C++ / Python / Julia / R)</b><br><sub>The same computation in four languages, compared while following its branch and loop.</sub></td><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-plots"><img src="docs/assets/demos/plots.svg" alt="The Plots demo running" width="100%"></a><br><b>Plots</b><br><sub>matplotlib code written for a window becomes the Card's display as it is.</sub></td></tr>
<tr><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-live-stream"><img src="docs/assets/demos/live-stream.svg" alt="The Live Stream demo running" width="100%"></a><br><b>Live Stream</b><br><sub>A time series plotted while the run is still going.</sub></td></tr>
</table>

#### Public data

<table>
<tr><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-casa"><img src="docs/assets/demos/casa.svg" alt="The Cas A · Chandra RGB demo running" width="100%"></a><br><b>Cas A · Chandra RGB</b><br><sub>An X-ray observation split into three energy bands for an RGB image, with the spectrum of the same photons to match lines to colours.</sub></td><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-gw150914"><img src="docs/assets/demos/gw150914.svg" alt="The GW150914 demo running" width="100%"></a><br><b>GW150914</b><br><sub>The chirp of a gravitational wave, extracted from public data and plotted.</sub></td><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-protein-6lu7"><img src="docs/assets/demos/protein-6lu7.svg" alt="The 6LU7 demo running" width="100%"></a><br><b>6LU7</b><br><sub>A protein's 3D structure, fetched to look at its binding pocket.</sub></td></tr>
<tr><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-exoplanet-transit"><img src="docs/assets/demos/exoplanet-transit.svg" alt="The Kepler-6 b demo running" width="100%"></a><br><b>Kepler-6 b</b><br><sub>A light curve folded to show an exoplanet's transit.</sub></td><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-seismic-trench"><img src="docs/assets/demos/seismic-trench.svg" alt="The Japan Trench demo running" width="100%"></a><br><b>Japan Trench</b><br><sub>A cross-section of hypocentres and the b-value, from an earthquake catalogue.</sub></td></tr>
</table>

#### Machine learning and computation

<table>
<tr><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-mnist-classifiers"><img src="docs/assets/demos/mnist-classifiers.svg" alt="The MNIST classifier comparison demo running" width="100%"></a><br><b>MNIST · comparing classifiers</b><br><sub>The real MNIST fed to seven classifiers at once, comparing accuracy and time.</sub></td><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-newton"><img src="docs/assets/demos/newton.svg" alt="The Newton's method demo running" width="100%"></a><br><b>Newton's method</b><br><sub>A map of the complex plane showing how the starting point decides where it goes and how fast.</sub></td><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-max-flow"><img src="docs/assets/demos/max-flow.svg" alt="The maximum flow and minimum cut demo running" width="100%"></a><br><b>Maximum flow and minimum cut</b><br><sub>On a made-up network, which edge to widen to increase the flow.</sub></td></tr>
<tr><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-pharmacokinetics"><img src="docs/assets/demos/pharmacokinetics.svg" alt="The pharmacokinetics demo running" width="100%"></a><br><b>Pharmacokinetics (a made-up drug)</b><br><sub>Teaching material comparing how concentration moves with the dosing interval. Not medical advice.</sub></td><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-dose-response"><img src="docs/assets/demos/dose-response.svg" alt="The dose–response demo running" width="100%"></a><br><b>Dose–response curve (synthetic data)</b><br><sub>A fit and its confidence interval, checked on data whose true values are known.</sub></td><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-molecules"><img src="docs/assets/demos/molecules.svg" alt="The molecular properties demo running" width="100%"></a><br><b>Molecular properties</b><br><sub>Properties and structural formulas from SMILES, with RDKit.</sub></td></tr>
<tr><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-cell-count"><img src="docs/assets/demos/cell-count.svg" alt="The cell counting demo running" width="100%"></a><br><b>Counting cells in images</b><br><sub>A counting method scored on synthetic images with known answers, then applied to real images of nuclei.</sub></td></tr>
</table>

#### Statistics in R

<table>
<tr><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-survival"><img src="docs/assets/demos/survival.svg" alt="The survival analysis demo running" width="100%"></a><br><b>Survival analysis</b><br><sub>How much the same hazard ratio moves with the subjects and the adjustment.</sub></td><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-meta-analysis"><img src="docs/assets/demos/meta-analysis.svg" alt="The BCG vaccine meta-analysis demo running" width="100%"></a><br><b>Meta-analysis: the BCG vaccine</b><br><sub>How the conclusion moves with the pooling method and the trials left out.</sub></td><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-rna-seq"><img src="docs/assets/demos/rna-seq.svg" alt="The RNA-seq airway demo running" width="100%"></a><br><b>RNA-seq: airway</b><br><sub>With DESeq2 and edgeR, how the number of differentially expressed genes changes with the threshold.</sub></td></tr>
<tr><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-simpson"><img src="docs/assets/demos/simpson.svg" alt="The Simpson's paradox demo running" width="100%"></a><br><b>A correlation that reverses when stratified</b><br><sub>Two real datasets in which the whole and its strata point in opposite directions.</sub></td></tr>
</table>

#### Simulation in Julia

<table>
<tr><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-battery"><img src="docs/assets/demos/battery.svg" alt="The battery scheduling demo running" width="100%"></a><br><b>Scheduling a battery</b><br><sub>Charging and discharging optimised by dynamic programming, under made-up prices.</sub></td><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-gillespie"><img src="docs/assets/demos/gillespie.svg" alt="The stochastic chemical reactions demo running" width="100%"></a><br><b>Fluctuations in chemical reactions</b><br><sub>A Gillespie simulation checked against theory.</sub></td><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-heat-inverse"><img src="docs/assets/demos/heat-inverse.svg" alt="The inverse heat conduction demo running" width="100%"></a><br><b>An inverse problem in heat conduction</b><br><sub>The naive inverse breaks down; regularisation recovers the answer.</sub></td></tr>
<tr><td width="33%" valign="top"><a href="https://gen740.github.io/Tsugumi/#demo-ecology-models"><img src="docs/assets/demos/ecology-models.svg" alt="The ecological model comparison demo running" width="100%"></a><br><b>Comparing ecological models</b><br><sub>Predator–prey models fitted to fur-trade records and compared.</sub></td></tr>
</table>

A Draft version of a demo is a project with only the shape of its Cards (names, descriptions and wires); you have the Agent implement it with "Generate Cards" ([The bundled demos](docs/Demos.md)).

## The flow of processing as a common language for people and AI

"What I want done", "what was run" and "what to change next".
Tsugumi shares all three on the same Workflow. From getting an intention across to weighing the result and asking the next question, the work moves forward on one structure.

This idea starts from research and extends to turning research decisions into teaching material you can run, to understanding and growing the tools you made with AI, and to handing work to the next person in a form they can run and check ([Vision and applications](docs/Vision.md)).

**The more work we hand to AI, the more we need an environment in which people understand the process and make the next decision.**

## Documentation

| Purpose | Documents |
| --- | --- |
| **Getting started** | [Installation](INSTALL.md) · [Usage](docs/Usage.md) · [Your first Card](docs/CardGuide.md) · [The bundled demos](docs/Demos.md) · [Web UI](docs/WebUI.md) · [Using it from Claude Code](docs/claude-code.md) |
| **The ideas** | [What to build so that we are not used by AI](docs/Philosophy.md) · [Why one Workflow for design, implementation and checking](docs/Rationale.md) · [Vision and applications](docs/Vision.md) · [Plans and guarantees](docs/Limitations.md) |
| **How it works** | [Features](docs/features.md) · [Versions](docs/Versions.md) · [Security and isolation](docs/security.md) · [The RPC contract](docs/Protocol.md) · [Glossary](docs/Glossary.md) |
| **Development** | [Development](docs/Development.md) · [Pinned versions](docs/Lock.md) · [Licenses and distribution](docs/LicenseAudit.md) |

The list of all documents is in [docs/README.md](docs/README.md).

## License

Tsugumi's own code is under the MIT License ([LICENSE](LICENSE)). External code, fonts and data are under their own licenses, whose full texts ship with the app and the `tsugumi` command. The list of dependencies is in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md); what the distributions include and how the VM's corresponding source is provided are in the [Licenses and distribution](docs/LicenseAudit.md).
