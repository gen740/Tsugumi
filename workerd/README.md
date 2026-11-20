# Worker ownership and direct local calls

`tsugumid` evaluates the definition and asks `tsugumi-workerd` for a worker.
Workerd owns ordinary, standby and resident slots. The core no longer owns a
`WorkerPool` or starts a Card process. Its local adapter still stages project
inputs, calls the borrowed worker connection, validates outputs and records the
run. Remote requests use the existing broker and TLS listener.

## Local connection handover

One lazily started `tsugumi-workerd --local` belongs to one core lifetime. The
core starts this service with an inherited Unix socket, not a public listener.
The private contract is [local_worker.fbs](../protocol/local_worker.fbs).
Acquire replies hand four descriptors (data, cancellation, stdout and stderr)
to the core with `SCM_RIGHTS`. WorkerCall, results, displays and logs flow directly
between core and worker; workerd never relays their bytes. Calls remain serial
inside a resident, with the call counter preserved across leases.

The core closes its borrowed channels before returning the lease. Workerd
retains process and scratch ownership, performs cleanup/termination requests,
and reuses a resident only after core validation. A successful Card call followed
by an output transfer failure keeps a living resident; Card failure, missing
required output, protocol loss and process death discard it. These are the
existing `worker_disposition` rules in `worker_execution.cc`, also used remotely.
The invocation engine takes a release callback so neither a second invocation
implementation nor a second local spawn path is needed.

Closing the management socket ends the pool, including active leases. This
covers a core crash because no worker inherits the socket. There is no local
journal, reconnect, restart recovery or retry. A run belongs only to the core
that started it. A workerd failure fails its core's worker requests rather than
silently creating another pool. `--workerd PATH` selects the service executable;
by default it lives beside the fallback `tsugumi-worker` executable.

Log readers pause before descriptors are lent and resume only after their
return. A wake pipe makes this handover immediate instead of waiting for the
previous 100 ms log poll. Deadline escalation still kills through workerd after
the existing one-second cooperative cancellation grace.

## Shared runtime and remaining differences

`tsugumi_worker_runtime` supplies worker process management, slots, WorkerCall
framing, invocation sequencing and resident disposition. `staging.cc` supplies
safe descriptor-relative output traversal, internal links, sparse copying and
rejection of escaping links/special files. Project storage and Nix evaluation do
not move into these helpers. The local client and remote launcher use the same
invocation engine. The core archive contains runtime objects for installed
consumers, but the local core adapter uses only borrowed connections.

| Stage | Local | Remote |
| --- | --- | --- |
| Build | Core realises in the shared store | Core exports derivation closure; remote realises in its private store |
| Admission | Workerd pool; standby and resident slots | Journal/global job cap, supervisor and namespace per job; still one-shot |
| Isolation | Darwin Seatbelt or Linux bwrap; existing sandbox/network/folder settings | Linux supervisor namespace and bwrap; existing remote restrictions |
| Input/output | Existing safe copies through shared filesystem paths | Blob snapshot/upload/download, hashes and path rewriting |
| Call | Borrowed descriptors; live display and bounded logs | Same runtime; status replies carry log slices from the core's cursor while it runs; the result carries the display and the authoritative log |
| Resident | Workerd retains until definition/environment ends or call fails | Remote resident slots remain to be implemented |
| Timeout | Separate build budget; optional Card budget starts after input staging | Existing journal deadline still includes queue/build/import; unification remains |
| Core death | Inherited management socket closes; pool ends | Core lifetime identity prevents picking up runs after a core restart; live-core network reconnection remains |

Grade selection and unified timeout/transfer progress are subsequent work; this
change does not claim they are implemented.

## Why this arrangement

The premise is the two existing execution adapters and the owner's requirement
that workerd own local slots while calls remain direct. Worker-loading, PATH,
resident and staging-race regressions demonstrate that these paths are used and
that their lifecycle matters. Descriptor handover removes core process ownership
without inserting a data relay. It cannot remove process startup or control IPC
cost: measure that cost with [tests/bench](../tests/bench/README.md). A local
journal would solve recovery after core death, which the owner explicitly ruled
out, so none is added.

## Verification

`LocalWorker.*` in `tsugumi_execution_service_tests` checks four-channel descriptor
handover, direct data after closing the management connection, CLOEXEC, empty
control messages, invalid frames and EOF of an unused service. These tests start
no Card process. Existing worker execution/staging tests retain their assertions.
The execution-service transfer-failure tests and `tests/core/integration/` cover
real resident lifecycle, input/output, cancellation and deadlines. Linux remote
integration is in `tests/remote` and requires namespaces and a packaged listener.
EOF with active/resident workers and real Darwin/Linux worker startup must also
be checked outside an environment that refuses worker sandbox creation.
