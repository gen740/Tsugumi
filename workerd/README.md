# Shared worker ownership and invocation (step 2)

`tsugumi_worker_runtime` is the single implementation of acquiring a worker,
calling it and returning it. Its objects are embedded once in `tsugumi_core` so
both the local service and the remote invocation launcher use the same engine,
including installed test consumers. There is no dependency on the scheduler,
Service, Nix evaluation or project storage. Neutral model/wire/process helpers
are supplied by the containing archive. Safe staging lives in `staging.cc`, in
the existing `tsugumi_workerd_support` library used by the runtime.

This step changes code ownership, not process ownership or behavior. The core
still starts local workers. Remote jobs still have their supervisor, namespaces,
private store and journal. No local journal or restart recovery is introduced.

## Caller surface

- `WorkerPool::acquire(card, key, spawn, reuse)` returns a lease with its worker
  connection (`WorkerProc`), reused flag and replacement reason.
- `workerd::run_invocation(pool, lease, card, key, reuse, invocation)` uses that
  connection and releases it on success or exception. The invocation contains
  concrete inputs, output port schemas, parameters, environment and limits.
- The core supplies project input snapshots and final output validation as
  callbacks; neither policy moves into worker ownership. `WorkerProgram` has
  only realised package/script fields, without build records or evaluation.
- `WorkerPool::release` retains or ends the connection; `clear` ends held workers.
  `worker_disposition` is the sole decision about retaining state after a call.
  A successful call followed by failed output transfer retains a living resident.
  Card failure, missing required output (core validation), rejected validation,
  unknown protocol state or process death cannot retain it.

The connection is still represented by an in-process object in this step. No
cross-process descriptor handover, new authentication or new storage contract is
implemented here.

## Step-by-step comparison

**E** means essential to separate machines/stores/folder views. **A** means an
accidental difference or existing policy that a later step may unify. The
original local sources were `core/src/{execution_service,worker_pool,worker_proc}.cc`
and `core/src/util/staging.cc`; remote adapters remain `workerd/src/invocation.cc`,
`remote_main.cc` and `journal.cc`.

| Step | Local path | Remote path | Classification and current owner |
| --- | --- | --- | --- |
| Resolve/build | Core evaluates and realises in host store before acquire; another system may use a built worker and emulator | Core stages a derivation archive; invoke realises in a persistent private store, serialized store operations | **E** separate Nix store and target system; adapters in `execution_service.cc`, `invocation.cc`, `nix_store.cc` |
| Admission/start | Acquire immediately, resident or fresh/standby; pool spawn thread outlives run lanes because bwrap tracks its spawning thread | Journal admission, global job cap, 100 ms listener start cycle; supervisor, unshare PID/user namespace and invoke process per job; one-shot acquire on invoke thread, no standby | **A** process arrangement and polling; `WorkerPoolMode::{standby,one_shot}`, `remote_main.cc` |
| Spawn environment | Fixed PATH=/var/empty, LANG and lifetime HOME/XDG caches; optional inherited certificate paths; definition's pinned shell/scripts, package and views | Same worker spawn, manifest shell and worker; private store mapped to /nix/store | Shared `worker_proc.cc`; **E** store mapping, **A** local fallback worker/emulator support in caller |
| Sandbox | Darwin Seatbelt (owner UID) or Linux bwrap (nobody when root/bwrap is available); existing unsandboxed option, per-Card network, allowed host folders, hidden core/control paths | Linux bwrap inside supervisor namespace; UID/GID 65534, sandbox required, no host folders | **E** OS and folder visibility; **A** policy restrictions remain in `graph.cc`, spawn adapters and supervisor. Shared enforcement is in `worker_proc.cc` |
| Handshake/loading | No separate ready handshake; acquire spawns channels, then first WorkerCall loads/prepares the Card | Identical | Shared `WorkerProc`; loading stays in `native_main.cc` |
| Input snapshot | Core's keep_file_value copies File/Directory through safe traversal to worker inputs/, fixed read-only mount; FlatBuffers sent directly | Core snapshots, hashes and uploads blobs; invoke copies job blobs into staged/, verifies input schema and rewrites logical paths, mounts snapshot read-only | **E** transfer/path mapping; **A** intermediate copy count. Core stage_inputs callback; `remote_invocation.cc`, `invocation.cc` |
| Invocation frame/env | Monotonic call_id, worker-owned work/tmp/out; manifest PATH, invocation HOME/LANG, run/Card/number/params/worker | Same, with existing environment ordering and transported params JSON | Shared `worker_execution.cc`, framing in `worker_proc.cc`; no new protocol |
| Logs/display | Worker pipes drained during call; bounded JSONL accounting, streams to core, displays delivered live; log budget excludes build | Same drain and budget mechanism; build and call share job log budget; logs and last display stored in result and imported at completion; module source transferred separately | **A** delivery and budget scope; Invocation log/finish callbacks, `execution_service.cc`, `invocation.cc`, core remote import. Forced supervisor kill can lose in-progress result/log delivery |
| Limits | 16 MiB call/result frames; standby defaults: 32, idle 600 s, RSS 2 GiB; residents have no age/count/RSS eviction. executionResources refused | Same frames; global max_jobs; optional delegated CPU/memory cgroup covers build too | **A** resource/admission policy; shared WorkerLimits, existing `graph.cc`, `remote_main.cc`. No extra filesystem policy is added: the setting is transported but unused by these adapters |
| Timeout | Optional invocation timeout after input staging; duration sent to worker; owner deadline gets 500 ms grace; build has its own timeout | Default 3600 s, at most seven days; journal deadline from acceptance includes queue/build/invoke/import; no per-call deadline passed into WorkerProc | **A**, preserved by Invocation timeout_ms/deadline_grace versus unset remote values; `journal.cc`, supervisor |
| Cancel | WorkerCancel control frame; 100 ms maximum control send, then 1 s grace before killing process group; partial send/protocol loss ends worker | Journal Cancelling; supervisor checks every 100 ms and kills namespace child; listener has 15 s identity-checked fallback | **A**, shared call cancel/deadline parameters versus external supervisor, `remote_main.cc` |
| Declared output staging | Pin out before call; keep worker alive for residents; copy only declared File/Directory, materialize internal relative links, reject escapes/special files, preserve sparse data. Forwarded input returns original path. Unknown/invalid outputs deferred to core validation | Pin out and input before call; terminate/drain worker before inspection; same safe copier; forwarded inputs copied under private .forwarded directory, paths rewritten relative to result/files. Unknown/invalid/outside paths rejected at adapter | **E** transferred result bytes/paths; **A** termination, forwarding copy and validation ordering. `OutputDelivery`, stop_before_outputs, shared stage_outputs and `staging.cc` |
| Return/failure | Core validates required outputs/types/paths before returning resident; clear work/tmp/out/inputs between calls, keep lifetime home/runtime state; transfer exceptions keep living resident after cleanup | One-shot worker always ends, no state retained; failed job removes staged result/files, journal imports result blobs then discards work scratch | **A** reuse/refusal and cleanup timing; shared worker_disposition/run_invocation/WorkerPool, `invocation.cc`, `journal.cc`. Missing physical file is a transfer error; an omitted required port is a Card validation failure |
| Owner/listener death | Pool ends with core; unfinished run becomes interrupted, no pickup or rerun | Supervisor can survive listener/core; journal identity/locks support recovery and result pickup; lost supervisor is interrupted, never automatically rerun | **A**, deliberately unchanged; core remote service and `remote_main.cc`/`journal.cc` |

## Why this extraction

The premise is two existing invocation adapters performing the same lifecycle
and staging operations. Divergence is real: the existing worker-loading/PATH,
staging-race and resident-transfer regression tests exercise those operations.
One runtime removes duplicate traversal, framing setup and retention decisions;
it does not claim to remove OS differences, transport costs or the behavioral
choices listed above. No speculative feature, restriction or recovery mechanism
is added.

## Verification

Existing worker loading, PATH, declared-output, staging and resident integration
tests retain their assertions. `worker_execution_test.cc` additionally runs both
output-delivery modes without a Card process: declared files/directories,
internal/escaping links, forwarded inputs, transfer failure versus Card failure,
carrier-specific validation, and resident disposition after validation/protocol
failure. `tsugumi_worker_staging_tests` still exercises the production copier,
including concurrent replacement and sparse files. Linux remote integration is
in `tests/remote`; it requires Linux namespaces and a packaged listener.
