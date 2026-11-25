# Card ABI

The worker calls a Card library at `lib/main.so`. The public contract is
[card_abi.h](../protocol/include/tsugumi/card_abi.h) and [card.fbs](../protocol/card.fbs).
SDKs turn the Card's schemas into language-level values and implement the entry point.
Core checks the fixed envelopes, sizes and filesystem boundaries.
The SDK interprets the payload bytes.

## Entry point

```c
int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result);
```

The call supplies a serialized `CardInput`, an opaque host pointer and an effects
table. A successful ABI call returns a serialized `CardResult` containing output
ports or a `CardFailure`. Parameters are embedded by the SDK at build time.
The input envelope contains only connected ports that arrived for this call.
It and its payloads are borrowed for the duration of the call.

The SDK returns a fresh libc `malloc` allocation in `result.data`. Ownership
passes to the host as soon as the entry returns, regardless of status. The
pointer must identify the start of the allocation. The worker releases it with
`free` after verification and after copying the fields needed for its reply.
This also applies to calls served by a resident worker.

## Fixed schemas

A `CardPort` carries `name`, `schema` and `bytes`.
Core stamps the schema ID from the producing port's declared type and writes the
same ID into the consumer's generated `_inputs.fbs`. As an implementation detail,
the ID is the lowercase SHA-256 of a canonical binary reflection schema rooted at
the port's type. Canonicalization keeps reachable declarations, sorted by fully
qualified name, and excludes documentation and filename metadata.

The SDK selects and verifies the input table using the schema ID. An unknown
schema is `input_invalid`. Each output is serialized with its own table as root.
An omitted optional output has no envelope. A missing `(required)` output is
`output_invalid`. Merge passes envelopes unchanged. Collect emits the fixed
`tsugumi.builtin.Collected` list of member envelopes with their original
schema IDs and bytes. Skipped members are absent.

## Artifacts

Include `tsugumi/artifact.fbs` to use `tsugumi.Artifact` in a port schema.
It holds a 16-byte random UUID. An artifact ID can travel in any value.
Core and the SDK pass the port envelope without a separate artifact list.

`artifact_create(host, flags, &id, &path)` returns a new UUID and a writable
directory. The worker seals its contents read-only at call completion.
Pass `TSUGUMI_ARTIFACT_KEEP` to retain the artifact after the run. The SDKs
pass KEEP by default. With flags set to zero, the artifact is available while
its run is active and expires when the run ends. Unknown flag bits fail.

`artifact_open(host, &id, &path)` opens any sealed artifact of this project by
UUID. Both effects return NUL-terminated paths scoped to the call and return
a negative value on error. Opening fails for an unknown, unsealed or expired
ID. Keep UUIDs within the intended audience, since anyone holding an ID in the
project can open its contents. A file is one member of an artifact directory.

The history label `<invocation sequence>.<counter>` identifies where an artifact
came from. APIs and evidence links use the UUID. See
[artifact access](SandboxContract.md#artifact-access) and
[storage and retention](RunHistory.md#keep-and-open-artifacts).

## Effects and their lifetimes

| Effect | Contract |
| --- | --- |
| `view_open` | Open a declared view and return its session and the opener's route. |
| `view_close` | Close the window by session UUID. Returns 0 when closed or already gone, and a negative value on error. |
| `route_register` | Create a route for the calling process. Returns 0 on success, `TSUGUMI_VIEW_WINDOW_CLOSED` (−2) when the session is no longer live, and −1 on other errors. |
| `route_unregister` | Drop a route held by the calling process. |
| `send` | Send bytes to one window by session. Returns 0 when accepted or dropped, and a negative value on error. |
| `recv` | Wait for an event on any route of the calling process. Returns 0 for an event, 1 for timeout, 2 for cancellation and a negative value on error. A negative timeout waits indefinitely. |
| `cancelled` | Report whether the invocation was cancelled. |
| `remaining_ms` | Return the remaining deadline in milliseconds, or −1 for no deadline. |
| `artifact_create` | Create a writable artifact directory. |
| `artifact_open` | Open a sealed artifact by UUID and resolve it to its read-only directory. |

The six view effects are available on any Card thread throughout the process
lifetime, including between calls in a resident worker. On each thread the
received bytes remain valid until its next receive. Outside the call, a cancelled
receive means the process is stopping. The other effects are scoped to the call.

## Display messages

The core assigns a random 16-byte UUID to each session and route. A session
addresses one window and can be passed to other Cards as data. A route addresses
one process mailbox for that window. The route lasts until it is unregistered or
the process ends. The user or any Card holding the session UUID can close the window.

Open with `TSUGUMI_VIEW_PANEL`, `TSUGUMI_VIEW_WINDOW` or
`TSUGUMI_VIEW_FULLSCREEN`. Pass zero for ordinary cursor behavior or combine
`TSUGUMI_VIEW_HIDE_CURSOR` and `TSUGUMI_VIEW_LOCK_POINTER` in the flags argument
after the mode. A locked pointer reports relative movement, and the user
can release it. The optional initial content size is in logical
pixels and applies to `TSUGUMI_VIEW_WINDOW`. Use two positive dimensions or
`0, 0` to let the UI choose. An unknown view, mode or flag, a partial size, or a
nonzero size in either of the other modes is an error.

`recv` returns the route, the session, and the event kind. Kind 0 carries
message bytes. Closing a window sends one kind 1 event with empty data to
each live route. Later sends to the closed session are dropped.
Without a presenting UI, opening a view still returns a session and a route.
Sends succeed and drop their messages. A Card can still close the session and
receive the close event.

Messages can carry up to `TSUGUMI_MESSAGE_MAX` (16 MiB). A larger send fails
immediately. The relay applies backpressure at each hop. A Card send waits
cancellably when the next hop lacks room for a maximum-size message. View
sends are nonblocking and address one route.

View traffic is transient. A Card can record any results it wants to keep in
output ports or logs.
See [declaring and hosting views](CoreAndSDK.md#views) and the
[SDK contract](https://github.com/gen740/tsugumi-kit/blob/main/docs/sdk-contract.md).
