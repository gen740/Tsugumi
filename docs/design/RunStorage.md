# Run storage and recovery

See [Run history and artifacts](../RunHistory.md) for inspection, recording policy and deletion controls.

## History storage

The primary database is `.tsugumi/runs.sqlite`. Created artifacts live under
`.tsugumi/artifacts/<run>/<card path>/@<invocation index>/<counter>/`. The database
maps UUIDs to their location, KEEP flag, size and sealed tree hash. Run History
can show IDs found by schema reflection in recorded values. IDs hidden in
opaque data, including Collect members and Any values, can still be opened by UUID.

[Storage paths and deletion controls](security.md#8-where-data-is-kept-and-how-to-delete-it).

## Startup cleanup

At startup, before accepting runs, Tsugumi removes artifact
directories in its managed layout with no artifact row, run scratch with no run
row, and leftover directories already marked for deletion. Tsugumi logs the
number of directories and bytes removed.

## Interrupted runs

A run belongs to the project core process that accepted it. Startup records an
unfinished invocation as `failed` with failure kind `interrupted`. It makes a
best-effort request to cancel the corresponding remote job. Uncollected
remote results remain on the worker for a person to inspect or remove.
[Remote job lifecycle](RemoteWorker.md#journal-and-scratch-lifetime) describes
reconnection and the acknowledgement that follows durable local completion.

An unkept artifact expires at run completion, including interrupted-run
recovery. The history transaction removes its record. The core detaches its
directory and reclaims the files outside the scheduler and history locks.
Startup cleanup covers an interruption between the record change and physical
deletion. Run and artifact deletion selected by a person use the same separation
between history changes and file reclamation.
