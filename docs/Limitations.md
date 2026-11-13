# Plans and guarantees

What Tsugumi can do today, and what is planned. This page gathers what the other documents say.

"What Tsugumi cannot guarantee in principle" lists what no tool can fundamentally solve ([Philosophy](Philosophy.md)). The rest is the current scope and the plans.

## What Tsugumi cannot guarantee in principle

- **A tool cannot guarantee that a result is scientifically right.** People judge that. What Tsugumi provides is a foothold from which the material for that judgement is within reach and other possibilities can be tried at once ([Philosophy](Philosophy.md#a-clean-result-alone-cannot-be-checked)).
- **Chance inside the computation cannot be pinned by pinning the environment.** Nix pins the environment and versions pin the code and definitions, so an analysis done once runs the same way on another Mac as long as the project is shared. Deciding conditions such as seeds, for processing whose result depends on random numbers, external data, arrival order or kept state, is up to whoever writes the computation ([Reproducibility](Rationale.md#reproducibility)).
- **Going back to an earlier version brings back code and definitions.** Not what was in a worker's memory then, nor data fetched from outside ([What is recorded, and where](Rationale.md#what-is-recorded-and-where)).

## Following results

- **Run comparison shows each Card's last invocation.** It uses retained records, so unrecorded inputs, outputs or parameters cannot be recovered from the current document. Large values and version diffs can be shown only in part; the comparison says when this happens.
- **Partial runs use saved inputs.** After changing something upstream, finish by running the whole Workflow ([Rerunning from the middle](Rationale.md#rerunning-from-the-middle)).
- **Whether past input and output values are kept is chosen per Card.** Values are kept for Cards with `recordIO` on (off by default; [What is recorded, and where](Rationale.md#what-is-recorded-and-where)).

[Data and permission boundaries](security.md) · [Supported platforms and installation](INSTALL.md) · [Browser hosting](WebUI.md)

## Features

- **Cards can be written in C++, Python, Go, Rust, C#, bash, R, Julia and Ruby.** R and Julia run in the same worker as C++ and Python, with live displays; like every Card they keep interpreter state only with `reuseWorker: true`. Julia Cards cannot use registry packages yet (`packages = [ ... ]` needs import-from-derivation, which the Card evaluator forbids). Their list/Dict interface converts values in memory through the worker’s typed FlatBuffers adapter.
- **A large demo (200 Cards) and WASM display are future extensions**.
- **The `load` of the science demos fetches from the publisher on every run.** If the publisher's content changes, so does the result. Cas A is the one pinned by hash ([The bundled demos](Demos.md#science-demos-fetching-and-analysing-public-data)).
