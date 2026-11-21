# The core and the Card SDK

**Status:** decided by the owner on 2026-11-13 and 2026-11-21; not implemented yet.
Each section states what is decided and what the code does today. Points that are not decided yet are listed under [Open](#open).

The Card ABI that connects the two is in [Card ABI 9](CardABI.md), and what a Card sees when it runs is in [The sandbox contract](SandboxContract.md).

## The core knows no language

**Decided.**

- The core is `tsugumid`, `workerd`, `hostd`, `protocol` and the UIs. None of them knows a programming language.
- A Card SDK is a swappable part, chosen per Card. It will later move to its own repository, so that users can build their own SDK.
- The contract between the core and an SDK is the common [Card ABI](CardABI.md).
- Bash, `preRun` and `postRun` live inside the SDK, not in the core.

**Today.**

- `_card.json` names a `language` from a fixed list (`python`, `cpp`, `go`, `rust`, `csharp`, `bash`, `r`, `julia`, `ruby`; [Code Card settings](CardGuide.md#code-card-settings)).
- The kernel runs a Bash Card's program directly, and Package Cards load `lib/main.so` ([Another system](features.md#another-system-x86_64-linux)).
- `preRun → run → postRun` are steps that the worker runs ([Worker state](CardGuide.md#worker-state)).
- hostd fixes one formatter per language ([Host presentation services](HostPresentation.md)), and `InputTypes` takes a `language` used to render SDK definitions ([Protocol](Protocol.md#inputtypes)).
- The SDKs live in [sdk/](../sdk/) of this repository, with the [Nix SDK](../sdk/nix/default.nix).

## What the core needs from an SDK

**Decided.** The core needs exactly these three things from an SDK:

1. **A Nix build entry** that returns a derivation containing `lib/main.so`.
2. **Static metadata:**
   - scaffold templates;
   - labels in Japanese and English;
   - a mapping from file extension to formatter and grammar;
   - the guide document;
   - descriptions of the input types.
3. **Per-Card SDK selection.**

**Today.** These are spread across the core: scaffolds live in `ui/shared`, labels in the UI's strings, formatters in hostd, and the guide in the Agent's prompts and in [CardGuide.md](CardGuide.md).

## Order of work

**Decided.**

1. Remove language knowledge from the core.
2. Rebuild the SDK with C++ as the reference.
3. Make the SDK self-contained in this repository.
4. Split it out into its own repository.

**Today.** None of these steps has started.

## Philosophy constraints that still hold

**Decided.** These hold through the whole change ([Philosophy](Philosophy.md#a-laboratory-notebook-and-who-decides-what-is-kept)):

- Run data travel only on `tsugumid`'s channel.
- Only a person deletes stored run data.
- Definitions (code and environment) may be shared by the machine.

**Today.** These already hold. See [Remote execution](RemoteExecution.md#data-left-on-a-worker) and [Where data is kept](security.md#8-where-data-is-kept-and-how-to-delete-it).

## Open

These questions are not decided yet:

- Where per-Card SDK selection is written: an `"sdk"` entry in `_card.json`, or `default.nix`.
- Whether a default SDK ships with the core until the split.
- The format of an SDK's static metadata: an executable, WebAssembly, or static data.
- Whether Cards whose body is Bash remain.
- Whether the core evaluates the manifest, `run`, exports and views.
