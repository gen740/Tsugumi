# Card ABI 9

**Status:** decided by the owner on 2026-11-13, 2026-11-21 and 2026-11-22. The ABI 9 cutover implements it and removes ABI 8 without a compatibility layer.
Points that are not decided yet are listed under [Open](#open).

The ABI is the contract between the core and a [language SDK](CoreAndSDK.md#terms): `tsugumi_main` in `lib/main.so` plus the FlatBuffers of [card.fbs](../protocol/card.fbs). The reference is [card_abi.h](../protocol/include/tsugumi/card_abi.h). What the worker builds around the call is in [The sandbox contract](SandboxContract.md).

## Entry point

```c
int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result);
```

- The call carries `{abi_version, input, host, effects}`. `input` is a `CardInput` FlatBuffer.
- `abi_version` comes first and is checked before any other part of the layout is read. A library built for another version fails cleanly.
- The host calls serially: one call at a time per process.
- A zero return writes a borrowed `CardResult` to `result`, including a result that carries a failure. The bytes stay valid until the next call.
- A nonzero return means that no result could be built.

## Fixed schemas

| Schema | Fields |
| --- | --- |
| `CardPort` | `name`, `kind`, `bytes`, `path` |
| `CardInput` | `inputs` for connected ports that arrived, and `params` as the generated `parameters.Params` buffer |
| `CardResult` | `outputs`, `failure` |
| `CardFailure` | `kind`, `message`, `source`, `line`, `cell` |

`source` is relative to the Card definition and `line` is one-based. Both are optional.

`PortKind::FlatBuffer` carries the port's buffer in `bytes`. `PortKind::Path` carries a file or directory in `path` and leaves `bytes` empty. The core checks the fixed envelope, ABI version, frame and byte limits, and filesystem boundaries. The SDK interprets and verifies the input, parameter and output payloads against their schemas.

## Path ports

- A Path input carries its staged absolute path for the duration of the call.
- A Card returns a new output as a relative or absolute path under its working directory. The worker determines at run time whether the path is a regular file or a directory. Directories may contain regular files and subdirectories. Symlinks and special files are rejected.
- **Forwarding** returns a staged input path unchanged. The worker records the original stored path without copying it, both locally and remotely.

## Built-in values

Branch reads a condition buffer using this fixed layout.

```fbs
namespace tsugumi.builtin;
table Condition { value:bool (id: 0); }
```

Collect emits the fixed table below. Each item preserves the member's name, kind, and opaque bytes or path in arrival order. Embedded paths refer to the original run data. Workers stage and remap paths carried by top-level Path ports.

```fbs
namespace tsugumi.builtin;
table Collected { items:[Tsugumi.Card.CardPort] (id: 0); }
```

## Collecting the result

- If the result is a failure, its outputs are discarded.
- A missing required output fails with `output_invalid`.
- Output data become stored run data at the moment `tsugumid` accepts them. How long they are kept follows the existing rule ([Philosophy](Philosophy.md#a-laboratory-notebook-and-who-decides-what-is-kept)):
  - the last run is kept by default;
  - older runs are kept only when a person keeps them explicitly;
  - only a person deletes.
- The collection order is part of the contract:
  1. The result is received.
  2. The worker copies every newly created output out as a snapshot. A reflink is allowed; a rename or a hardlink is not. Forwarded inputs are not copied.
  3. Only then may the worker clear scratch, send the next call, or be destroyed.
- A snapshot copies each file at collection time. There is no atomic guarantee across a directory. The language SDK's documentation requires the Card to finish writing before it returns.
- Each call has budgets for its total output bytes and its number of entries. They are project settings, 64 GiB and 1,000 entries by default. Forwarded inputs do not count. Going over fails the call with `output_invalid`.
- Remote workers follow the same rules.
- Clearing scratch after a call does not clear the working directory.

## What a run records

- There is no automatic deletion. Only a person deletes.
- A run records these categories:

  | Category | What | Setting |
  | --- | --- | --- |
  | Run summary | Success or failure, the failure's details, timings and the versions used | Always recorded |
  | Logs | stdout and stderr | On or off |
  | Values | Input and output values | `recordIO` |
  | Files and artifacts | Path values and artifacts | `retainArtifact` |

- Displays are not recorded. Nothing sent through `send` reaches history, traces, remote result files or exported apps. What to keep from views is decided after the views work.
- Each category except the run summary can be turned on or off. The project sets the defaults, and each Card may override them per category.
- The log size limit is a project setting.
- A person can delete records from the history tree for each run and each category.
- Structured data, such as FlatBuffers values, live in SQLite. Path values and artifacts stay in the filesystem.

The cutover does not implement the per-category log setting yet. Logs are always recorded, up to 128 MiB per invocation.

## Effects and their lifetimes

The effects table has exactly three entries:

```c
int (*send)(void* host, const char* view, const uint8_t* bytes, size_t size);
int (*cancelled)(void* host);
int64_t (*remaining_ms)(void* host); /* -1: no deadline */
```

- **`send`** delivers opaque bytes to one of the Card's views, named by `view`.
  - The bytes' format is the private contract between the sender and that view. Nobody in between reads them.
  - The host consumes or copies the bytes before `send` returns.
  - `send` and its `host` context stay valid for the life of the process. A Card's own thread may call it between calls and after a call has returned.
  - The core does not check the view name. The UI matches it against the Card's [declared views](CoreAndSDK.md#views) and shows an error for an unknown name.
  - Zero means the host accepted the message. Nonzero means the host could not take it.
- **`cancelled`** and **`remaining_ms`** belong to the current call. They and their context may be used only during a call, and they must not reenter the SDK's own cancellation or deadline queries.
  - Cancellation state is process-wide, not thread-local.
  - Outside a call, the language SDK's `cancelled()` and `remaining_ms()` return true and 0 without calling the host.
- The Card never sees an identity. `tsugumid` stamps each message with the Card and the run that is active when it arrives, and `WorkerCall` carries no run, invocation or Card.

## Display messages

- A message travels Card → worker (`WorkerDisplay{view, bytes}`, independent of calls) → `tsugumid` → the project subscription (`DisplayUpdated`) → the UI → the view.
- `tsugumid` adds the Card and, when a run is active, its `run_id`. A message that arrives between runs is shown live with no run.
- `DisplayRunStateChanged` tells the UI when the Card's part in a run starts and ends.
- When a subscriber falls behind, only the latest pending message per Card and view is kept. Control events keep their order.
- A message is not attached to an invocation, and a message after the call has returned is not an error.
- A frame with a stale or unknown `call_id` does not kill the worker.

## Failures

- A failure is returned in `CardResult.failure`, with return code 0.
- The worker reports an invalid result envelope as a fixed failure. The SDK reports payload decoding errors.

## What the ABI does not have

- JSON;
- reflection or `bfbs` inside `main.so`;
- phases, an init step, run-start hooks or a `Run` argument;
- a `run_id`, invocation or Card that the Card can see;
- fixed display kinds, modules, configs, blobs or a record flag.

## The Card's shape in every language

- In every language a Card is `(input) -> output`. There is no `Report` object.
- Side effects go through global functions of the language SDK. In the C++ SDK:
  - `card::display::send(value)`, overloaded by value type, which encodes the value for one of the SDK's views;
  - `card::display::send(view, bytes)` for any declared view;
  - `card::cancelled()` and `card::remaining_ms()`.
- The other SDKs have the same three operations in their own spelling. Their guides are in [tsugumi-kit](https://github.com/gen740/tsugumi-kit).

## Open

These questions are not decided yet:

- An opt-in accessor for the raw tables.
