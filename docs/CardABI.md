# Card ABI 9

**Status:** decided by the owner on 2026-11-13 and 2026-11-21; not implemented yet. ABI 9 replaces ABI 8.
Each section states what is decided and what the code does today. Points that are not decided yet are listed under [Open](#open).

The ABI is the contract between the core and a [Card SDK](CoreAndSDK.md). What the worker builds around the call is in [The sandbox contract](SandboxContract.md).

## Entry point

**Decided.**

```c
int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result);
```

- The call carries `{abi_version, CardInput bytes, host, effects}`.
- `abi_version` comes first and is checked before any other part of the layout is read.
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
| `CardInput` | `inputs`, `params` |
| `CardResult` | `outputs`, `failure` |

## File and Directory outputs

**Decided.**

- The Card creates the file or directory itself and writes its path into the matching member of the output struct.
- There is no output directory, and the input carries no target path.
- An output file is a regular file, not a link, under the Card's working directory.
  - Paths under `TMPDIR` and granted paths are not accepted.
  - A Directory output is a directory under the working directory that contains only regular files.
- **Forward** is allowed. When a Card returns an input path unchanged, the path is accepted, the original stored path is recorded, and nothing is copied. Forwarded input paths are excluded from the snapshot copy below, so only newly created outputs are copied. A symlink that resolves to an input is rejected.

**Today.**

- File and Directory outputs are written beneath `run.output_directory` (`$TSUGUMI_OUTPUT_DIR` in Bash), and the Card returns their paths ([Building results](CardGuide.md#building-results)).
- The core copies them from the worker's output staging folder after a successful call ([Security §4](security.md#sandbox-true-the-default)).

## Collecting the result

**Decided.**

- If the result is a failure, its outputs are discarded.
- A missing required output fails with `output_invalid`.
- Output data become stored run data at the moment `tsugumid` accepts them, and part of the run's record. How long they are kept follows the existing rule ([Philosophy](Philosophy.md#a-laboratory-notebook-and-who-decides-what-is-kept)):
  - the last run is kept by default;
  - older runs are kept only when a person keeps them explicitly;
  - only a person deletes.
- The collection order is part of the contract:
  1. The result is received.
  2. The worker copies every newly created output out as a snapshot. A reflink is allowed; a rename or a hardlink is not.
  3. Only then may the worker clear scratch, send the next call, or be destroyed.
- A snapshot is a copy of each file, taken at collection time. There is no atomic guarantee across a directory: a Card thread that is still writing gets whatever each file holds when it is copied. The SDK documentation requires the Card to finish writing before it returns.
- Each call has budgets for its total output bytes and for its number of entries.
- Remote workers follow the same rules.
- Clearing scratch after a call no longer clears the working directory (`work/`).

**Today.**

- `WorkerProc::clear_scratch` empties the work, temporary, output and input folders after each call (`workerd/src/worker_proc.cc`).
- The result frame is limited to 16 MiB ([Building results](CardGuide.md#building-results)). There is no budget for entry count.

## Effects and their lifetimes

**Decided.**

- The effects are exactly these three:
  - **display**: screen output, in the one display form below. The Card never sees an identity;
  - **`cancelled`**;
  - **`remaining_ms`**.
- Cancellation state is process-global, not thread-local.
- `tsugumid` wraps each display with the Card's and the run's identity; the worker does not. `WorkerCall` drops `run_id`, `invocation` and the Card's identity: the worker needs none of them.
- Lifetimes:
  - The `display` callback stays valid for the life of the process.
  - `host` and the ABI callbacks `cancelled` and `remaining_ms` may be called only during a call.
  - Outside a call, the SDK globals `cancelled()` and `remaining_ms()` return true and 0 without calling the host.

**Today.**

- The ABI 8 function table also carries logging, failures and JSON conversion.
- The worker fills the Card, `run_id` and invocation of each `Display` (`protocol/data.fbs`), and `WorkerCall` carries them.
- The SDKs give `Report` and `Run` objects whose callbacks belong to one invocation. A retained one rejects later calls: for example C# throws `ObjectDisposedException` ([C#](CSharp.md)), and Python's Matplotlib display does nothing outside a call (`sdk/python/tsugumi/mpl.py`).

## The display form

**Decided.**

- Every display is a view module of Wasm and/or JavaScript. Inside it, the module is free to use the DOM, a canvas or WebGPU.
- `Data.Display` becomes module + config + blobs.
- Today's fixed kinds become default view modules that the core ships on the UI side ([Displays in the UI](CoreAndSDK.md#displays-in-the-ui)). The kinds are text, markdown, scalar, table, image, points, line chart, Plotly and Vega-Lite.
- The SDK's overloads, such as `card::display(Table{...})`, stay. Inside, they encode the data for those default modules.
- These are removed:
  - the `Text` … `PointCloud` tables of `protocol/data.fbs`;
  - the display kinds of the ABI;
  - the worker's `display_*_impl` functions.

**Today.**

- `DisplayValue` in `protocol/data.fbs` is a union of `DisplayText`, `DisplayScalar`, `DisplayTable`, `DisplayImage`, `DisplayPointCloud` and `DisplayModule`.
- ABI 8 has the display kinds `TSUGUMI_DISPLAY_TEXT`, `MARKDOWN`, `SCALAR`, `LINE_CHART`, `TABLE`, `IMAGE`, `POINTS` and `MODULE` ([abi.h](../sdk/include/tsugumi/sdk/abi.h)), and `workerd/src/native_main.cc` implements them in `display_*_impl`.
- Plotly, Vega-Lite and the line chart are modules in [sdk/nix/builtin-views.nix](../sdk/nix/builtin-views.nix).

## Display after the call returns

**Decided.**

- A display that a Card's own thread sends after the call has returned is not an error, and it can keep updating. This widens what Cards can do.
- It is appended, not overwritten, to the invocation that was started most recently when it arrived, together with its arrival time. The history therefore shows that it came after the return.
- Each display call says whether that display is kept in the record, through an argument of `display` ([The Card's shape](#the-cards-shape-in-every-language)). When the argument is omitted, the display is **not** recorded.
- A frame with a stale `call_id` must not kill the worker.

**Today.**

- `on_display` in `tsugumid/runtime/src/scheduler.cc` drops a display whose run, Card or invocation is not the current one.
- Each display replaces the previous one. Only the latest display of each invocation, and of each Card in a run, is stored (`History::put_display`, `INSERT OR REPLACE`).
- Earlier displays of an invocation are shown live and not recorded. No option chooses this: it follows from replacement.
- A frame with an unknown `call_id` fails the call with `worker_failed` (`workerd/src/worker_proc.cc`).

## Failures

**Decided.**

- A failure is returned in `CardResult`, with return code 0.
- A non-zero return code means that no result could be built.
- An unknown kind or bad bytes become a fixed `CardFailure` kind.

**Today.** ABI 8 records a structured failure through its function table, and `tsugumi_main` then returns non-zero.

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

- In every language a Card is `(input) -> output`. There is no `Report` object.
- Side effects go through global functions of the SDK. In C++:
  - `card::display(...)`, overloaded by value type, for example `card::display(Table{...})`, a scalar or an image. Argument-dependent lookup may be used;
  - `cancelled()` and `remaining_ms()`, beside it.
- The per-call record flag is an argument of `display`.

**Today.** Entry points take up to three roles, `inputs`, `report` and `run`, as listed in [Inputs, results and reports](CardGuide.md#inputs-results-and-reports).

## Open

These questions are not decided yet:

- An opt-in accessor for the raw tables.
