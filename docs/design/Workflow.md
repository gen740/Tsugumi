# Cards and Workflows

Every Card has an id for its definition directory, connections and run records. Host allocates a lowercase id and adds suffixes such as `_2` when needed. The `title` in its README is the display name. See [Card ids and port names](../Reference.md#card-ids-and-port-names).

## Kinds of Card

Every Card states its kind in `_card.json` as `"type"`: `code`, `draft`, `trigger`, `merge`, `collect` or `subgraph`. Nothing else decides it; a missing or unknown `type` is a definition error.

| Kind | Role |
| --- | --- |
| Code Card (`code`) | Builds its processing in `default.nix` with a language SDK into `lib/main.so` (C++, Python, Go, Rust, C#, Julia, R, Ruby, Java, JavaScript or an SDK of your own) |
| trigger | Starts a Workflow and emits an empty value on `out`. May supply Nix exports to its downstream Cards |
| Merge | Forwards every arriving envelope to `out` |
| Collect | Aggregates a batch into a `Collected` list of envelopes |
| subgraph (Card in Card) | Handles a child Workflow as one Card. `boundary` maps the outer ports to Cards inside |
| Draft (`draft`) | An "unimplemented Card": write its name and content directly on the card, and connect it just by dropping a wire on it (it has no ports; the other end names the real port). A wire end on a Draft names only the Card, with no `port`. Implementing one requires explicitly rebinding its wires to real ports in the same save. The Agent can later generate its implementation |

A Card in Card uses full Card paths to distinguish state and history; visual groups in `layout.bin` do not affect execution.

## Typed ports

- An output port can feed several inputs. Each input port accepts one wire. Use a Merge Card to bring several arrivals to one downstream input.
- An output port's type is declared in `_outputs.fbs`. Each connected input inherits the type of its upstream output. The SDK checks the types a Card uses.
- A Merge forwards each value unchanged. Its consumer's generated `_inputs.fbs` uses a single type or a union of distinct candidate types. The Canvas displays their names as `A | B`.
- Mark an output `(required)` when every invocation must produce it. Omit an optional output to select the downstream paths that run. The SDK interprets the bytes in the fixed [Card ABI](CardABI.md) envelope.

## Execution semantics (scheduler)

- A Card runs when every connected input has arrived. An unconnected input is absent from the call. An arrival is a single value or a batch of values. A Merge consumes one arrival at a time.
- Bounded queues with backpressure, cancellation and a per-invocation deadline when `timeoutSeconds` is greater than zero. Each input port holds up to 16 unconsumed groups, and a run that can no longer progress fails with `execution_limit`. A Run can request concurrency from 1 to 64, with up to eight Code workers active at once. The iteration limit (`maxRuns`) counts how often the Card receives a set of inputs that triggers it.
- When a connected input cannot arrive because its producer omitted the output or skipped, the downstream Card is recorded as `skipped`. Merge skips once all of its inputs are exhausted without any arrival. A Card downstream of a failed, blocked or cancelled Card is `blocked`, with a reason. A Draft, and the Cards that need only its outputs, do not run and get no state; the canvas marks them as not run because of a Draft. Independent paths keep running.
- The core selects a worker for each Code Card invocation. See [Worker selection](RemoteWorker.md#worker-selection) and [run recovery](RunStorage.md#interrupted-runs).
- Partial runs supply saved inputs to connected ports whose producers are outside the run. A downstream Merge consumes the values that arrive during that run. [Run history](../RunHistory.md) explains partial-run provenance and recording.

## Run lists and Collect

A Code Card can return a list of `{ label; run; }` variants from `default.nix`. Use a list in `params.nix` to configure the variants. For each trigger, the runtime runs every combination of the Card's variants and the items on its connected inputs. Two implementations and batches of three and four inputs give 24 invocations. Labels identify members in run history and comparisons.

Collect aggregates a batch into one fixed `Collected` value, preserving each member's schema ID and payload bytes. Skipped members are absent. Collect emits an empty list when there are no members. Consumers open artifact UUIDs contained in members by passing them to the SDK.

The runtime limits one trigger to 256 combinations and a run to 4,096 worker invocations. The input and output batches each allow up to 32 MiB of serialized port values.
