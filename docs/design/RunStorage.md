# Run storage and recovery

See [Run history and artifacts](../RunHistory.md) for inspection, recording policy and deletion controls.

## History storage

The primary database is `.tsugumi/runs.sqlite`. Created artifacts live under
`.tsugumi/artifacts/<run>/<card path>/@<invocation index>/<counter>/`. The database
maps UUIDs to their location, KEEP flag, size and sealed tree hash. Run History
can show IDs found by schema reflection in recorded values. IDs hidden in
opaque data, including Collect members and Any values, can still be opened by UUID.

[Storage paths and deletion controls](security.md#8-where-data-is-kept-and-how-to-delete-it).

## Run scratch

Run scratch is a working folder, not run data. Each invocation has its own
scratch: the Card's `TMPDIR` and `/tmp`, the worker's working folders for a
fresh worker, the core's per-invocation folder under the project's run scratch
root, and a remote job's folder on its worker. The runtime erases it when the
invocation ends, whether it succeeded, failed, was cancelled or was
interrupted. It erases after the outputs, values and logs are recorded and the
artifacts are sealed into `.tsugumi/artifacts`, so nothing the record needs
stays behind. A resident worker (`reuseWorker`) keeps its working directory as
[worker state](../CardGuide.md#worker-state), not as scratch.

Scratch is tmp and is cleaned up by design; to keep files, a Card mounts a host
folder (`grants.hostPaths`). Reruns use the recorded inputs in history, never
an earlier run's scratch.

Run data (records, values, logs and artifacts) is still never deleted
automatically. Only an unkept artifact, which exists for its run, expires with
it.

## Startup cleanup

At startup, before accepting runs, Tsugumi removes artifact
directories in its managed layout with no artifact row, all run scratch left
by an earlier process, and leftover directories already marked for deletion.
Tsugumi logs the number of artifact directories removed.

## Interrupted runs

A run belongs to the project core process that accepted it. Startup records an
unfinished invocation as `failed` with failure kind `interrupted`. It makes a
best-effort request to cancel the corresponding remote job and then releases
it, which erases the job's scratch on the worker once the job has stopped.
[Remote job lifecycle](RemoteWorker.md#journal-and-scratch-lifetime) describes
reconnection and the acknowledgement that follows durable local completion.

An unkept artifact expires at run completion, including interrupted-run
recovery. The history transaction removes its record. The core detaches its
directory and reclaims the files outside the scheduler and history locks.
Startup cleanup covers an interruption between the record change and physical
deletion. Run and artifact deletion selected by a person use the same separation
between history changes and file reclamation.
