# Card workers

`tsugumid` evaluates the Card definition and asks `tsugumi-workerd` for a
worker. The build plan supplies the selected package and, for foreign and
remote execution, the worker executable built for the same system. Workerd
owns standby and resident processes. The core stages project inputs, calls
the worker, and consumes its result.

## Local connections

One `tsugumi-workerd --local` belongs to one core lifetime. The core starts the
service with an inherited Unix socket. The private
[management contract](../protocol/local_worker.fbs) passes data, cancellation,
stdout and stderr descriptors to the core via `SCM_RIGHTS`. Calls remain
serial per process, with the counter preserved across leases.

The core reads the data stream throughout the lifetime of its borrowed
connection, including between leases of a resident. Logs return to workerd
when the lease is released. The reader hands opaque display messages to the
Card's display sink and wakes the caller only for its terminal reply.
Workerd keeps process and scratch ownership, including evictions and
termination after the one-second cancellation grace.

Closing the management socket ends the pool and its active leases. A
workerd failure fails the core's worker requests. `--workerd PATH` selects
the service executable. By default it lives beside the fallback
`tsugumi-worker`.

## Invocation boundary

The worker calls `tsugumi_main` with an array of names, schema IDs and borrowed
input byte spans. The language SDK decodes each arrived value with the
generated schema and requests result storage through `result_alloc`. Launch
secrets arrive in `WorkerStart`. The work directory and `TMPDIR` are fixed at
launch.

A packed resident worker receives a writable result mapping and read-only
input mappings over the inherited data connection. Shared input spans name
byte ranges in those mappings. The result reply reports its initialized size
and the core keeps the mapping alive while readers hold any of its outputs.
All Cards in an exported app come from its author and honor the handoff when
they return. Development workflows still verify the full result envelope and
seal artifact snapshots.

Artifact grants come from input envelopes. The worker exposes writable
directories through `artifact_create`, seals their snapshots at call
completion, and resolves active grants through `artifact_open`. Forwarding
an artifact ID preserves its identity. See the [Card ABI](../docs/CardABI.md).

View effects can run on Card threads throughout the process lifetime.
The worker relays bytes in both directions and uses flow control to bound
pending messages.

## Remote execution

The broker sends authenticated requests to the TLS listener. The listener
admits jobs through its journal and starts each invocation in its own
supervised Linux namespace. The invocation transfers input blobs, realises
the Card build plan in the private Nix store, and runs the worker specified
by the plan. The invocation engine checks artifact IDs and transfers newly created artifacts with its result. Forwarded grants retain their IDs.

## Verification

`tsugumi_native_contract_tests`, `test_worker_loading.py`, and
`test_worker_loading_boundary.py` exercise the worker ABI and package
loading. `LocalWorker.*` in `tsugumi_execution_service_tests` exercises
descriptor handover, idle display delivery and stale replies. The staging
and invocation tests cover output snapshots, forwarding, budgets and
process reuse. The Linux tests in `tests/remote` exercise the real listener,
namespaces and remote Card results.
