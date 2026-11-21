# Card ABI 9

**Status:** decided by the owner on 2026-11-13 and 2026-11-21; not implemented yet. ABI 9 replaces ABI 8.
Each section states what is decided and what the code does today. Points that are not decided yet are listed under [Open](#open).

The ABI is the contract between the core and a [Card SDK](CoreAndSDK.md).

## Entry point

**Decided.**

```c
int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result);
```

- The call carries `{abi_version, CardInput bytes, host, effects}`.
- The host borrows the `CardResult` that the Card writes to `result` until the next call.

**Today.** ABI 8 ([abi.h](../sdk/include/tsugumi/sdk/abi.h), `TSUGUMI_ABI_VERSION 8`) has the same entry name. Its call carries:

- a `phase`, which handles interpreter preparation, source loading, the run-start hook and the call itself;
- `run_id` and `invocation`;
- a function table with one FlatBuffer per port, a `parameters.Params` FlatBuffer, logging, structured failures and typed JSON conversion (`inputs_json`, `params_json`, `output_json`).

## Fixed schemas

**Decided.**

| Schema | Fields |
| --- | --- |
| `CardPort` | `name`, `kind`, `bytes` |
| `CardInput` | `inputs`, `params`, and one file path for each File or Directory output port. The Card writes that port's result to the path |
| `CardResult` | `outputs`, `failure` |

There is no output directory: each File or Directory output port gets its own target path in `CardInput`.

**Today.** File and Directory outputs are written beneath `run.output_directory` (`$TSUGUMI_OUTPUT_DIR` in Bash), and the Card returns their paths ([Building results](CardGuide.md#building-results)).

## Effects

**Decided.**

- The effects are only:
  - display, with a `DisplayValue`, which the worker wraps with the Card's identity and the run's identity;
  - `cancelled`;
  - `remaining_ms`.
- Cancellation state is process-global, not thread-local.

**Today.** The ABI 8 function table also carries logging, failures and JSON conversion. The SDKs give `Report` and `Run` objects whose callbacks belong to one invocation thread ([R and Julia worker lifecycle](CardGuide.md#r-and-julia-worker-lifecycle)).

## What the ABI does not have

**Decided.** The ABI has none of these:

- JSON;
- reflection or `bfbs` inside `main.so`;
- phases, an init step, `on_run_start`, or a `Run` argument;
- a `run_id`, invocation or Card that the Card can see.

**Today.** ABI 8 has all of them:

- JSON conversion, used to return values from R, Julia and Ruby;
- phases;
- `on_run_start` / `onRunStart` / `OnRunStartAsync` ([Presentation and hooks](CardGuide.md#presentation-and-hooks));
- a `Run` object with run and Card IDs, the invocation number and working and temporary directories ([Inputs, results and reports](CardGuide.md#inputs-results-and-reports)).

## The Card's shape in every language

**Decided.**

- In every language a Card is `(input, [report]) -> output`.
- Cancellation and remaining time are globals of the SDK.
- A failure is returned in `CardResult`, with return code 0. A non-zero return code means that no result could be built.
- R, Julia and Ruby will later use generated, typed C++ glue.
- How values are mapped inside an SDK is that SDK's business.

**Today.** Entry points take up to three roles, `inputs`, `report` and `run`, as listed in [Inputs, results and reports](CardGuide.md#inputs-results-and-reports).

## Open

These questions are not decided yet:

- How `[report]` is made optional in C++: an overload chosen by archive member, or always passing `Report`.
- An opt-in accessor for the raw tables.
- What `cancelled()` and `remaining_ms()` return outside a call.
