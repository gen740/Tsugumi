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

The worker checks the nested input and parameter FlatBuffers before calling
the entry point in `lib/main.so`. The Card returns a binary `CardResult` with
typed outputs or a failure. Launch secrets arrive in `WorkerStart`. The
worker keeps a fixed work directory, input staging, and `TMPDIR`. Input and
temporary staging are cleared between successful calls. Files in the work
directory remain available to a resident Card.

New File and Directory outputs come from the working directory. The
invocation engine snapshots declared files and directory trees using pinned
directory descriptors and follows no symbolic links. Forwarded inputs
retain their original identity. For new outputs the default budget is
64 GiB of logical file data and 1,000 filesystem entries per invocation.

The worker drains process-lifetime `send(view, bytes)` messages without
opening a view or reading its declarations. Local workers relay them
directly to the core. Remote job results contain typed outputs and the
bounded log.

## Remote execution

The broker sends authenticated requests to the TLS listener. The listener
admits jobs through its journal and starts each invocation in its own
supervised Linux namespace. The invocation transfers input blobs, realises
the Card build plan in the private Nix store, and runs the worker specified
by the plan. The same invocation engine checks output bytes and snapshots
new files. Remote forwarding uses the original job-relative input
path, so result transfer does not duplicate the input.

## Verification

`tsugumi_native_contract_tests`, `test_worker_loading.py`, and
`test_worker_loading_boundary.py` exercise the worker ABI and package
loading. `LocalWorker.*` in `tsugumi_execution_service_tests` exercises
descriptor handover, idle display delivery and stale replies. The staging
and invocation tests cover output snapshots, forwarding, budgets and
process reuse. The Linux tests in `tests/remote` exercise the real listener,
namespaces and remote Card results.
