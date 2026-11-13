# Why one Workflow for design, implementation and checking

**The specification a person draws, the thing an AI edits and the processing that runs share one structure.**
What sets Tsugumi apart is that this relationship holds all the way through Drafts, Cards, types, environments and versions.

For a person to understand an analysis an AI has pushed forward, and to try the next condition, the intention, the code and the results have to be tied together. Tsugumi treats that tie as the Workflow: the diagram of the processing is what gets implemented, and after a run it is where you check the results and make changes.

The [README](../README.md#are-you-using-ai-in-your-research-as-much-as-you-could) named three reasons we hesitate to hand research to AI: reproducibility is not assured, the path to the conclusion is invisible, and handing data to AI is frightening. This page answers them in turn. Each section ends with **Where to check**: the screens, demos, code and tests that show the answer holds. Operations are in [Usage](Usage.md) and the details of each mechanism in [Features](features.md).

| Hesitation | Section | In one line |
| --- | --- | --- |
| The path is invisible | [The drawn specification is where you implement and check](#the-drawn-specification-is-where-you-implement-and-check) | What a person reads, what AI edits and what runs are the same |
| | [Why instruct AI with Drafts and Workflows](#why-instruct-ai-with-drafts-and-workflows) | The links between steps travel more exactly as a diagram than as prose |
| | [Rerunning from the middle](#rerunning-from-the-middle) | Fix just the Card you were curious about and check it at once |
| Reproducibility | [Reproducibility](#reproducibility) | Nix pins the environment; each Card decides the conditions its result depends on |
| | [What is recorded, and where](#what-is-recorded-and-where) | Each Card chooses what to keep; git holds definitions, the run history what ran |
| Handing over data | [Unpublished data](#unpublished-data) | Computation stays in the VM on the Mac; what leaves it is fixed |

## The drawn specification is where you implement and check

**What a person looks at to understand, what the AI edits and what actually runs are the same Workflow.**

Hand an analysis to AI and the trials go fast, but you receive the process only as a written report. A detailed report is too long to read through; a short summary loses the decisions made along the way and the conditions tried. Holding the report up against the code is left to you.

Take an analysis that reads observations, estimates the background noise, fits a model and plots the result. You start by placing each step as a Draft and wiring them up, so what goes into which step is shared before any code is written. The Agent reads the descriptions and the wires and writes each Card's code, output types and environment. The input types downstream are derived from the output types, so the contract between connected steps agrees. After a run, if the background estimate looks doubtful, open that Card, change it and try it with the saved inputs; when it is right, run the whole to confirm.

**The "here" in "I want to change this here" points at the same place in the specification, the code and the run.** That shortens the distance between understanding an analysis and finding where to change it.

A large step can be gathered into a child Workflow and handled as one Card most of the time; when needed you go inside and check it in finer units. However much the processing grows, you can survey it at the granularity you want to think at. Failed attempts, and branches that tried other conditions, stay in the version tree next to the steps that were adopted.

You do not read the AI's report and believe it; you go straight to the place you are curious about and run it yourself.

## Why instruct AI with Drafts and Workflows

**Because the links between steps travel more exactly as a diagram than as prose.**

As in the [README's example](../README.md#what-we-want-to-ask-of-ai-should-be-simple), what you want may be simple enough to draw as three boxes, yet asking in prose turns into long instructions and long reports. With ten steps, branches and merges, writing out in prose which output goes to which input is harder still, and even the writer cannot see what is missing or contradictory.

- A **Draft** is a Card with only a name and "what it does", not yet implemented. Press `E` to place one and write it; wire it up and its ports follow the wires.
- The diagram of Drafts is the specification: press "Generate Cards" at the top right to have the Agent implement it.
- After implementation, output types decide the input types downstream, and a mismatched wire shows up as a diagnostic before anything runs.
- One output carries one wire. To send a value to several Cards it goes through a fork Card, so the distribution is visible on the diagram as a Card too.

Intentions are written in words and relationships as wires, so people and AI point at the same place as they design, implement and check.

**AI takes on the work of building a structure that is easy to check.** Pinning environments, defining input and output types and splitting the processing all make checking easier, but if a person had to arrange them by hand every time, even a short trial would carry that preparation. From the purpose written in a Draft and its connections, the Agent implements all of it: it defines the environment in `default.nix`, writes the output types in `_outputs.fbs`, writes the processing in C++ / Python / bash / R / Julia, and fixes it while running it. **Because implementation can be left to AI, a structure people can check easily can be there from the start of an analysis.** That is why Tsugumi takes this shape now. People state the purpose of each step and how they relate, weigh the implementation and results, and ask the next question.

The Agent and the UI go through the same operations and the same validation rules of the core. Cards the AI saved stay as ordinary project files that people can open and edit, and a finished Workflow runs without an LLM.

## Reproducibility

**Nix pins each environment, and each Card decides the conditions its result depends on.**

To check a Card on its own, it has to be clear what it takes, where it runs and what it returns.

| Mechanism | What it makes clear | What it lets you do during a trial |
| --- | --- | --- |
| Typed ports | The structure of the data passed between steps | Check the types of wires before running and derive the input types downstream |
| A Nix definition per Card | The dependencies for building and running the code | Keep the environment's definition with the version and reuse built Nix outputs |
| Cards and child Workflows | The extent of the processing you change and check | Survey the whole, and go inside a step when needed |
| Saved inputs and run history | The inputs for the next trial, and the record of the run | Fix part of it and try again, and look at logs and results |

Analysis software can require complex environments. Tsugumi uses Nix to pin each Card's dependencies and reuse its build results.

- Each Card's environment (its language toolchain and libraries) is built by Nix from that Card's `default.nix`. The definition stays with the project's version, so the same environment can be built on another machine.
- A heavy computation (a long simulation, say) can be a Nix build with its seed and conditions written in. While the definition and the dependencies do not change, its output is reused from the Nix store, so the later analysis or plotting can be tried any number of times.
- The same environment does not by itself guarantee the same result. If a result depends on random numbers, external data, arrival order (an `any` Card) or state kept by a resident worker (only a Card that sets `reuseWorker: true` has one; by default every invocation starts fresh), the Card has to decide that condition: make the seed a parameter, write per-run initialisation in `onRunStart` / `on_run_start`, and so on.

## What is recorded, and where

**Each Card chooses what to keep, and git and the run history keep different things.**

Keeping every invocation's inputs and outputs unconditionally would make storage balloon at once for stream and loop Cards, and make the trials themselves heavy. So what is kept is divided like this:

| Kept | Default | Chosen by |
| --- | --- | --- |
| Run state, logs and displays (figures and tables) | Always | — |
| The last input of each port, for partial runs | Always | — |
| Each invocation's input and output values | Not kept | The Card's `recordIO` |
| Artifacts (files and folders a Card wrote) | Not kept | The Card's `retainArtifact` |
| The run history written into the project (`.tsugumi/runs.sqlite`) | Written | The project's `saveRunHistory` |

Invocation work folders are scratch, separate from retained run history. The most recent run’s scratch remains available for inspection; starting another run of that project removes the earlier scratch. This does not remove retained run data: recorded File / Directory values, declared artifacts and the last inputs for partial runs are copied to their own retained locations before completion. Startup removes scratch whose run no longer exists. Scratch is disk-backed and private to the project’s user, and its size is shown separately in Build Cache settings.

Even with `recordIO` off, the last inputs used for partial runs are kept ([Rerunning from the middle](#rerunning-from-the-middle)). Computation results you will keep using belong in Nix outputs; data you want to look at during trials belongs in the history or the artifacts.

| Where | What it keeps | What it is for |
| --- | --- | --- |
| Git (the project's folder) | The Workflow's definition, the code, params and the environment's definition | Following changes and starting another trial from an earlier version |
| Run history (SQLite) | Run state, logs, displays and, as configured, input and output values | Seeing what actually ran and what it produced |

- When a run starts and the content has changed since the last version, the saved content is committed and that commit is recorded with the run. The same version can be run many times, so commits and runs are one to many.
- When a run ends, one line with the result and a short summary of what ran is added to that commit's note (see it with `git log --notes=tsugumi`).
- With "Version summaries" on, a model writes a short sentence about the version's changes into another note. Hover a version in the version tree to see the sentence and a minimap of the Cards.
- "Keep as Version" gives the current content a name (a tag) and a memo.
- "Start from This Version" in the version tree keeps the current content as a version and then returns to that version's content. The previous line of work is not lost.

What comes back is code and definitions, not the state of workers at the time or all the data.

**Tsugumi versions the code and the Workflow's definition with git and ties each run to its version.** From the version tree you can go back to earlier content and start a different attempt, and logs and displays are in the run history. Keeping the code, reading the results and trying the next condition become one line of research. The operations are in [Versions](Versions.md).

## Rerunning from the middle

**To make the back-and-forth of fixing and checking one Card at a time fast.**

While you build an analysis, you often want to fix just this Card's processing or change just the conditions of a plot. Running everything from upstream each time makes the back-and-forth slow.

In Tsugumi you can run only the Cards you choose, or from there downstream, with the latest saved inputs.

- Use "Run Only This Card (Last Inputs)" or "Run from Here Downstream" in the context menu. Child Workflows can be tried the same way.
- Going downstream from one branch, a merging Card fills the ports that do not receive anything this time with saved inputs, so you can compare again without rerunning the other branches.
- If a needed input has not been saved yet, the run fails clearly; run once including what is upstream.

Two cautions:

- **If you changed something upstream and have not run it, the saved inputs are still the old ones**, as are the values used at a merge. Which run's values were used is kept in the run's journal.
- **Finally, run the whole Workflow to confirm.** A partial run is for trying a part while you build. Taking an old input does not bring back the code or upstream conditions of that time.

## Unpublished data

**It can be handled without giving it to AI. A Card's computation stays inside the VM on the Mac, and what leaves it is fixed.**

- Cards run in an isolated Linux VM inside the Mac. Cards have no outside network by default.
- Without the Agent, with "Version summaries" off and with no Card that has `network: true`, the project's content and data do not leave the Mac and the VM (apart from Nix fetching packages).
- "Version summaries" are off by default. When on, the code and settings that changed in each version are sent to the model's provider; data files are not.
- With the Agent, the conversation and the sources, logs and input and output values the Agent reads are sent to the model's provider. There is no mechanism yet that masks what is sent.
- A save in which the Agent gives a Card the network or a folder of the Mac does not go through without the user's approval.
- A finished Workflow runs without an LLM. You can build the processing with the Agent on public or synthetic data, and run it on unpublished data without the Agent.

## Measures that keep trial and error light

Processing is split into small Cards so that people can understand the decisions along the way and work on them. To run repeatedly at that granularity, the cost of preparing, starting and saving, and the handling of inputs in partial runs, are designed in too.

### Not rebuilding the same code every time a condition changes

In analysis you often try a threshold or a display condition over and over without changing the code. If all of these were build inputs, splitting the processing finely would mean waiting for rebuilds.

Tsugumi separates `params.json`, used at run time, from `params-nix.json`, passed to Nix, and keeps run-time-only parameters out of the build inputs. `_meta.json` and `_card.json`, which hold a Card's description and settings, are not passed whole either; the build gets the sources, types and Nix parameters it needs. Reusing build results is left to Nix's derivations and store.

The layout on screen is kept apart from the computation's definition, too: a save that changes the computation while a run is going is refused, but moving Cards is accepted. **Trying a condition, changing code and tidying the screen each cause only the work they need.**

### Reusing the evaluator and workers so small steps can repeat

If a computation of a few lines split into a Card started the Nix evaluator or a language runtime every time, preparation would take longer than the computation.

The Nix evaluator is built into `tsugumid`, and the evaluated state of nixpkgs and the daemon connection are reused across Cards and runs. C++, Python, R and Julia Cards are called by a worker that loads their shared library. By default each invocation gets a fresh worker, so that a Card keeps no hidden state, and the next worker is started and its library loaded ahead of time; a Card that sets `reuseWorker: true` keeps one resident worker, with its interpreter and module state, across invocations.

A reused worker's state carries across runs, and the initialisation it needs can be written in `onRunStart` / `on_run_start`. On failure, cancellation, a deadline or a memory limit, the worker is ended. **The cost of starting is lowered, while computations that keep state, and when they initialise, stay explicit.**

### Partial runs without gathering inputs again

To fix and try a step in the middle you need what was input just before. Saving only a path is useless once the original temporary file is gone, and mixing inputs from separate invocations gives different conditions from last time.

Tsugumi saves the full set of inputs of the latest invocation and the latest input of each port.

- **Sets are kept whole.** An `all` Card keeps the set used in one invocation; an `any` Card uses each port's latest value.
- **Files are kept too.** File / Directory values are copied into storage, so they can be used after the run cache is cleared.
- **Checked before use.** On a rerun, the inputs are checked against the current input types and for the files' existence, and if something is missing the reason is returned.
- **For child Workflows too.** Running a child Workflow alone, these inputs are passed in at its boundary.
- **Compare again at a merge.** Going downstream from one branch, only the ports of a merging Card that receive nothing this time are filled with these inputs, so the new value of the branch you changed is compared with the values of the other branches. Which run's values were used is kept in the run's journal.

**Researchers can try a change where they were curious without gathering inputs by hand.** These latest inputs are saved separately from the full input and output history chosen with `recordIO`.

### A saved record behind every "finished"

If downstream steps went ahead as soon as a step returned a value, a later failure to save the record would leave a run that shows as successful on screen but cannot be traced.

Tsugumi validates the outputs, keeps the artifacts the settings ask for and saves the record of the invocation's completion before it announces completion and passes values downstream. A value whose save failed is not passed on. The input and output values recorded keep the type schema of that moment. The stages shown on screen are not guessed from the final state either; they come from the stages the core actually recorded: evaluation, build, run, validation and saving.

**How far a run got and where it failed are seen on screen and in the run history from the same evidence.** The full history of input and output values and the keeping of artifacts are settings, so the amount recorded can be tuned to the analysis.

### Keeping Nix dependencies consistent with a Workflow that branches

When a Card refers to an upstream Card's Nix definition, being reachable by following wires backwards does not mean that Card runs first.

Say A branches to B or C, and D runs on whichever result arrives. The code Card that always precedes D is A. One of B and C does not run, so if D could refer to both definitions unconditionally, the dependencies would disagree with the execution path drawn on screen.

Tsugumi analyses how inputs are awaited, branches and merges, loops and child Workflows, and finds the Cards that precede on every path. `all` takes the union of what its required inputs guarantee; `any`, and several paths into one port, take the intersection; loops are solved as a greatest fixed point. Only the definitions of code Cards that meet this condition are passed to the Nix evaluator, and those referred to are evaluated lazily.

References written directly in a source are also diagnosed on save as `nix_reference_*`, and an indirect reference (through an alias, say) fails to evaluate, because the evaluator is given only the definitions in range. **The Workflow's connections are reflected not only in the order of execution but also in the dependencies of the environment's definitions.**

## What it brings together from existing approaches

Trying code interactively, representing processing as a graph, and managing dependencies and versions are each ideas found in existing approaches. Tsugumi connects them like this:

| Approach | How Tsugumi connects it |
| --- | --- |
| Run code and think while looking at the results | From a Card's result, open its processing and try a change with saved inputs |
| Ask AI in natural language to implement something | Add the connections to the Drafts' descriptions, and share the same Workflow after implementation |
| Represent the relationships of processing as a graph | Make the displayed connections the basis of type derivation and the rules of execution |
| Manage versions of environments, code and experiments | Tie runs to versions of the code, and start the next attempt from a past version |

Each mechanism behind this has its own document: Card isolation in [Security and isolation](security.md), the worker that calls a C++ Card as a shared library in [Card SDK and workers](features.md#4-card-sdk-and-workers), the rules for branches, merges and loops in [Execution semantics](features.md#execution-semantics-scheduler), and saving as text files in [Documents and saving](features.md#5-documents-and-saving).

**What Tsugumi aims for is a research environment in which people and AI design, run and keep thinking on the same structure.**
With that structure, researchers can return to the trials an AI pushed forward and test their own questions as concrete changes.
The idea of extending this relationship from research to learning, development and handing over work is in [Vision and applications](Vision.md); the background is in [What to build so that we are not used by AI](Philosophy.md).
