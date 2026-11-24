# Card ABI

The worker calls a Card library at `lib/main.so`. The public contract is
[card_abi.h](../protocol/include/tsugumi/card_abi.h) and [card.fbs](../protocol/card.fbs).
SDKs turn the Card's schemas into language-level values and implement the entry point.
Core checks the fixed envelopes, sizes, artifact grants and filesystem boundaries.
The SDK interprets the payload bytes.

## Entry point

```c
int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result);
```

The call supplies a serialized `CardInput`, an opaque host pointer and an effects
table. A successful ABI call returns a serialized `CardResult` containing output
ports or a `CardFailure`. Parameters are embedded by the SDK at build time.
The input envelope contains only connected ports that arrived for this call.

## Fixed schemas

A `CardPort` carries `name`, `schema`, `bytes` and a vector of artifact IDs.
Core stamps the schema ID from the producing port's declared type and writes the
same ID into the consumer's generated `_inputs.fbs`. As an implementation detail,
the ID is the lowercase SHA-256 of a canonical binary reflection schema rooted at
the port's type. Canonicalization keeps reachable declarations, sorted by fully
qualified name, and excludes documentation and filename metadata.

The SDK selects and verifies the input table using the schema ID. An unknown
schema is `input_invalid`. Each output is serialized with its own table as root.
An omitted optional output has no envelope. A missing `(required)` output is
`output_invalid`. Merge passes envelopes unchanged. Collect emits the fixed
`tsugumi.builtin.Collected` list of member envelopes with their original IDs,
bytes and artifact grants. Skipped members are absent.

## Artifacts

Include `tsugumi/artifact.fbs` to use `tsugumi.Artifact` in a port schema.
It holds a 16-byte ID and can appear in tables, vectors or unions. The SDK lists
every ID in the output envelope, including IDs nested in the payload.

`artifact_create` returns a new ID and a writable directory. The worker seals
its contents read-only at call completion. `artifact_open` returns a read-only
directory for an ID granted in the call's input envelopes. Both return
NUL-terminated paths scoped to the call. A file is one member of an artifact
directory. Forward a granted ID to pass the same artifact without copying it.
Listing any ID outside the call's created and granted sets causes
`output_invalid`. See [storage and retention](RunHistory.md#keep-and-open-artifacts).

The core allocates a project-wide invocation sequence and supplies it to the
worker. A created ID is the sequence followed by the worker's zero-based
artifact counter, encoded as two big-endian 64-bit integers. It is displayed
as `<invocation sequence>.<counter>`. Access depends on the recorded grant.
`artifact_open` checks the call's grant and materializes its directory on
demand. Input Card artifacts retain their existing IDs and store paths. See
[artifact access in the sandbox](SandboxContract.md#artifact-access).

## Effects and their lifetimes

| Effect | Contract |
| --- | --- |
| `send` | Send bytes to a named view. Returns zero when accepted and a negative value on error. |
| `recv` | Wait for bytes from a named view. Returns 0 for a message, 1 for timeout, 2 for cancellation and a negative value on error. A negative timeout waits indefinitely. |
| `cancelled` | Report whether the invocation was cancelled. |
| `remaining_ms` | Return the remaining deadline in milliseconds, or −1 for no deadline. |
| `artifact_create` | Create a writable artifact directory. |
| `artifact_open` | Resolve a granted artifact to its read-only directory. |

Send and receive are available on any Card thread throughout the process
lifetime. On each thread the received bytes remain valid until its next receive.
Outside the call, a cancelled receive means the process is stopping. The other
effects are scoped to the call.

## Display messages

Each message can carry up to `TSUGUMI_MESSAGE_MAX` (16 MiB). A larger send fails
immediately. SDKs can split larger values and reassemble them in the view. The
relay applies backpressure and a Card send waits cancellably when the worker
has no room for a maximum-size message. With no open receiver, core discards
the message. View sends are nonblocking.

TODO(open): Record results that depend on view interaction.
See [declaring and hosting views](CoreAndSDK.md#views) and the
[SDK contract](https://github.com/gen740/tsugumi-kit/blob/main/docs/sdk-contract.md).
