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

The call supplies an array of `tsugumi_input` descriptors, an opaque host pointer
and an effects table. Each descriptor names a connected port that arrived for the
call and carries its schema ID and a span of payload bytes. The array, strings
and spans are borrowed until the entry returns. Parameters are embedded by the
SDK at build time.

A successful ABI call returns a serialized `CardResult` containing output
ports or a `CardFailure`. The SDK requests writable storage with
`result_alloc(host, size)` and returns the base pointer and initialized length
in `result`. The effect can succeed once per call. The host can lend process
memory or a shared mapping. Once the entry returns, the storage belongs to the
host regardless of status. The Card must neither free the storage nor access
it after returning. The host releases it after its last reader finishes.

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
| `view_open` | Open or reuse a declared view, register the calling process, and return the session and a new route. |
| `view_close` | Close the window by session UUID. Returns 0 when closed or already gone, and a negative value on error. |
| `route_register` | Register the calling process on the session and return a route. Returns 0 on success, `TSUGUMI_VIEW_WINDOW_CLOSED` (−2) when the session is no longer live, and −1 on other errors. |
| `route_unregister` | Drop a route held by the calling process. |
| `send` | Send bytes to one window by session. Returns 0 when accepted or dropped, and a negative value on error. |
| `recv` | Wait for an event on any route of the calling process. Returns 0 for an event, 1 for timeout, 2 for cancellation and a negative value on error. A negative timeout waits indefinitely. |
| `cancelled` | Report whether the invocation was cancelled. |
| `remaining_ms` | Return the remaining deadline in milliseconds, or −1 for no deadline. |
| `artifact_create` | Create a writable artifact directory. |
| `artifact_open` | Open a sealed artifact by UUID and resolve it to its read-only directory. |
| `result_alloc` | Lend an aligned buffer for a result up to the host's message limit. Returns null on failure. |

A view sends to a project-root Card path, or to `"*"` for all Cards
registered on its window. Each matching live process receives the bytes
once. Route UUIDs identify Card-side registrations for `recv` and
`route_unregister`.

The six view effects are available on any Card thread throughout the process
lifetime, including between calls in a resident worker. On each thread the
received bytes remain valid until its next receive. Outside the call, a cancelled
receive means the process is stopping. The other effects are scoped to the call.

## Display messages

The core assigns a random 16-byte UUID to each session and route. A session
addresses one window and can be passed to other Cards as data. A route addresses
one process mailbox for that window. The route lasts until it is unregistered or
the process ends. The user or any Card holding the session UUID can close the window.

Within a run, a Card path, view name, and run-list entry identify one live
session. An entry is identified by its label when present or by its index
in the run list otherwise. Each successful
`view_open` returns a fresh route even when it reuses the session.
The first open sets the mode, flags, and size. Later opens ignore those
arguments while the session is live. Opening after a close gives the Card a
new session. Between calls in a resident process, `view_open` uses the last
invocation's run and variant.

Open with `TSUGUMI_VIEW_PANEL`, `TSUGUMI_VIEW_WINDOW` or
`TSUGUMI_VIEW_FULLSCREEN`. Pass zero for ordinary cursor behavior or combine
`TSUGUMI_VIEW_HIDE_CURSOR` and `TSUGUMI_VIEW_LOCK_POINTER` in the flags argument
after the mode. A locked pointer reports relative movement, and the user
can release it. The optional initial content size is in logical
pixels and applies to `TSUGUMI_VIEW_WINDOW`. Use two positive dimensions or
`0, 0` to let the UI choose. When creating a session, an unknown view, mode
or flag, a partial size, or a nonzero size in either of the other modes is an error.

`recv` returns the route, the session, and the event kind. Kind 0 carries
message bytes. Closing a window sends one kind 1 event with empty data to
each live route. Later sends to the closed session are dropped.
Without a presenting UI, opening a view still returns a session and a route.
Once a UI subscribes, the next `view_open` for that live session presents it
with the original view and window settings. Sends before presentation succeed
and drop their messages. A Card can close a headless session and receive the
close event.

Messages can carry up to `TSUGUMI_MESSAGE_MAX` (16 MiB). A larger send fails
immediately. The relay applies backpressure at each hop. A Card send waits
cancellably when the next hop lacks room for a maximum-size message. View
sends are nonblocking and address Card paths on the window.

View traffic is transient. A Card can record any results it wants to keep in
output ports or logs.
See [declaring and hosting views](CoreAndSDK.md#views) and the
[SDK contract](https://github.com/gen740/tsugumi-kit/blob/main/docs/sdk-contract.md).
