# The core and the Card SDK

**Status:** decided by the owner on 2026-11-13 and 2026-11-21; not implemented yet.
Each section states what is decided and what the code does today. Points that are not decided yet are listed under [Open](#open).

The Card ABI that connects the two is in [Card ABI 9](CardABI.md), and what a Card sees when it runs is in [The sandbox contract](SandboxContract.md).

## The core knows no language

**Decided.**

- The core is `tsugumid`, `workerd`, `hostd`, `protocol` and the UIs. None of them knows a programming language.
- A Card SDK is a swappable part, chosen per Card. It will later move to its own repository, so that users can build their own SDK.
- A Card selects its SDK in its `default.nix`, by the SDK that gets injected into it, not in `_card.json`. This leaves room for third-party SDK implementations without tying the Card document to Nix evaluation.
- The contract between the core and an SDK is the common [Card ABI](CardABI.md).
- Bash, `preRun` and `postRun` live inside the SDK, not in the core.

**Today.**

- `_card.json` names a `language` from a fixed list (`python`, `cpp`, `go`, `rust`, `csharp`, `bash`, `r`, `julia`, `ruby`; [Code Card settings](CardGuide.md#code-card-settings)).
- The kernel runs a Bash Card's program directly, and Package Cards load `lib/main.so` ([Another system](features.md#another-system-x86_64-linux)).
- `preRun → run → postRun` are steps that the worker runs ([Worker state](CardGuide.md#worker-state)).
- hostd fixes one formatter per language ([Host presentation services](HostPresentation.md)), and `InputTypes` takes a `language` used to render SDK definitions ([Protocol](Protocol.md#inputtypes)).
- The SDKs live in [sdk/](../sdk/) of this repository, with the [Nix SDK](../sdk/nix/default.nix).

## What the core needs from an SDK

**Decided.** There is no SDK metadata. The interface between the core and an SDK is only:

1. **a Nix build entry** that returns a derivation containing `lib/main.so`;
2. **[Card ABI 9](CardABI.md).**

What an SDK might otherwise have described lives elsewhere:

- Highlighting by file extension is UI presentation.
- Formatting is the Agent's job, or the Card's `default.nix` can expose a formatter.
- The guide is ordinary documentation in the SDK's repository.
- The generated header (`card.hh`) is the description of the types.
- These are removed from the core:
  - hostd's `DescribeInput`;
  - the `FormatLanguage` enum;
  - the per-language formatter tables.

**Today.**

- hostd renders input types per language with `sdk::DescribeInput` (`hostd/src/input_types.cc`).
- `FormatLanguage` in `protocol/tsugumi_enums.fbs` lists `cpp`, `python`, `bash`, `nix`, `json` and `fbs`.
- hostd fixes one formatter per language ([Host presentation services](HostPresentation.md)).
- The guide is in the Agent's prompts and in [CardGuide.md](CardGuide.md).

## Templates

**Decided.**

- Templates are independent of the SDK, and of the core and the UI.
- There are template stores at two levels:
  - the host;
  - the project (local).
- Users can add templates.
- A "make template" skill may create templates at the project level.
- Templates can be injected from git.
- The core manages the list: it stores, lists and copies templates, and imports them from git.
- The Agent or the user chooses a template. The core never chooses: there is no automatic selection and there are no per-language defaults.
- Scaffolding a new Card is the Agent's job.

**Today.** These are to be removed:

- the fixed language lists, such as `language` in `_card.json` ([Code Card settings](CardGuide.md#code-card-settings));
- [ui/shared/templates](../ui/shared/templates/), which holds one scaffold per language.

## Order of work

**Decided.**

1. Remove language knowledge from the core.
2. Rebuild the SDK with C++ as the reference.
3. Make the SDK self-contained in this repository.
4. Split it out into its own repository.

**Today.** None of these steps has started.

## The bundled SDKs

**Decided.**

- R, Julia and Ruby will later use generated, typed C++ glue. This is part of the plan for the SDKs bundled with Tsugumi, not a constraint of the ABI.
- How values are mapped inside an SDK is that SDK's business.

**Today.** R, Julia and Ruby return values that the worker converts from JSON to each port's FlatBuffer ([Building results](CardGuide.md#building-results)).

## Philosophy constraints that still hold

**Decided.** These hold through the whole change ([Philosophy](Philosophy.md#a-laboratory-notebook-and-who-decides-what-is-kept)):

- Run data travel only on `tsugumid`'s channel. This covers Tsugumi's own transport. I/O that a Card starts through its grants is a permitted side effect ([Grants](SandboxContract.md#grants)).
- Only a person deletes stored run data.
- Definitions (code and environment) may be shared by the machine.

**Today.** These already hold. See [Remote execution](RemoteExecution.md#data-left-on-a-worker) and [Where data is kept](security.md#8-where-data-is-kept-and-how-to-delete-it).

## Open

These questions are not decided yet:

- Whether a default SDK ships with the core until the split.
- Whether Cards whose body is Bash remain.
- Whether the core evaluates the manifest, `run`, exports and views.
