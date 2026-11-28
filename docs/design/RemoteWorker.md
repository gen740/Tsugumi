# Remote execution and worker selection

The host's `tsugumid` evaluates a Card's Nix definition for the system the worker runs it as (the worker's native system when the Card lists it, otherwise the Card's first system). The core publishes the part of the derivation's input closure that no pull cache serves to a binary cache. The worker obtains it through signed substitution, realises the outputs by substitution or building, runs the Card in a sandbox and returns typed outputs, files, logs and provenance. What the Card sends to its views travels live through the same connection and is never written to the job's result files. Workers never evaluate Card definitions.

The [worker contract](../../protocol/remote_worker.fbs) defines framed FlatBuffers requests and transfer limits; implementation is in [workerd/](../../workerd/). The broker lives in host-side `tsugumi-hostd`. The Mac's Virtualization.framework adapter only copies bytes from vsock port 1027 to hostd; the Web host gives project daemons hostd's private Unix endpoint. The broker relays access to the host-managed [binary cache server](BinaryCacheServer.md) on the authenticated job connection.

[Install and register a remote worker](../RemoteExecution.md#install-and-register).

A worker's capabilities carry its `worker_contract` and the source `revision` it was built from (the flake's revision, set by the `workerd` package's wrapper; empty for other builds). Hostd records them whenever it verifies the worker or forwards a probe. A verified worker whose contract differs from this Tsugumi's is *out of date*: settings say so, the catalog lists it as unavailable with that reason, and a forwarded probe that finds it is refused with the same sentence after the record is updated. Worker selection names it in the failure as `worker:<id>` = `outdated`, with `contract:<id>` (the worker's contract) and `contract` (this core's), so the run fails before any probe is built when no other worker can take the Card. Requests for jobs already on the worker, such as releases, are still forwarded. [Update a worker](../RemoteExecution.md#update-a-worker).


## Dashboard

The listener serves its [dashboard](../RemoteExecution.md#the-workers-dashboard) on its own TCP listener, `dashboard_port` on the same `listen_address`, over HTTPS (TLS 1.2 or 1.3) without client certificates. The page is one self-contained HTML document (`workerd/dashboard/`, embedded at build time with its English strings) that calls a JSON API on the same origin:

| Request | Who | Does |
| --- | --- | --- |
| `GET /` (`?lang=ja` or `en`, else `Accept-Language`) | anyone | the page |
| `GET /api/state` | anyone | the worker, jobs, disk, settings, host keys with their last connection since start, login state, and for an administrator the session's CSRF token |
| `POST /api/login`, `/api/logout` | anyone | start or end an administrator session |
| `POST /api/jobs/remove` | administrator | the CLI's removal without `--active` |
| `POST /api/keys/add`, `/api/keys/remove` | administrator | edit `authorized_keys` |
| `POST /api/settings` | administrator | check and save `settings.json` |
| `POST /api/password` (`current_password`, `new_password`, `confirm_password`) | administrator | verify the current password, replace it and end all other sessions |
| `POST /api/restart` (`after_jobs` or `now`) | administrator | restart the listener |

Once the listeners are bound and before publishing readiness, the daemon generates a 20-character password if `STATE/admin-password` is absent, saves its salted hash and prints the initial password once to standard output. Initialization, the CLI reset and the dashboard's verify-and-replace operation serialize on `STATE/admin-password.lock`. The dashboard uses the same session, CSRF and origin checks for a password change as for its other administrator operations. Login and password changes share the listener's password lockout.

`Capabilities` carries `dashboard_port` and `dashboard_certificate_sha256`. Hostd turns them into `ExecutionCapabilities.dashboard_url` (`https://` with the registered endpoint's host) and the certificate hash, which the Mac app's dashboard view pins. Hostd relays nothing: the UIs load the page from the worker.

A restart after the running jobs starts no further job and restarts once no supervisor runs. A restart now cancels the running and handed-off jobs as a host's `CancelRequest` would, then restarts. Either way queued jobs stay queued for the new listener, and sessions end.

## Job execution

The worker runs at most `max_jobs` jobs at once across all project scopes (default eight; see [worker settings](../RemoteExecution.md#worker-settings)). A change saved from the dashboard applies at the listener's next scheduling pass to the jobs that start from then on, and probes advertise the value in effect. Additional accepted jobs wait durably. `buildTimeoutSeconds` bounds the build; `timeoutSeconds` bounds only the Card call, exactly as for local execution. A value of zero means no deadline. Waiting for a free worker and moving input, derivation or result files consume neither Card-call time nor build time. There is no aggregate job deadline. A running invocation is never automatically rerun: a listener restart can adopt a surviving supervisor through its lock within the same submitting core life; a lost supervisor becomes interrupted. Cancellation of an unknown job creates a fingerprint tombstone, preventing a late submission from starting it.

At startup the listener reads the job journals. During normal operation,
journal-lock events and supervisor process descriptors wake job scheduling.
An unreadable job journal cannot take a worker slot. Upload retries compare
overlapping bytes with staged data and charge only new bytes against the
upload budget.

The core supplies each job's recording budget in `JobSpec.log_limit_bytes`. The budget limits the recorded output. Remote workers send the build log and the Card's output in size-prefixed FlatBuffer entries and report recording truncation separately. The Card's stdout and stderr are one pipe, so its output is one stream in the order it wrote, as on a local worker.

The log arrives while the job runs. Each status request, and each cancellation the running invocation sends, carries the core's cursor: the number of log bytes it has received. The reply returns at most 256 KiB from there and the log's current size; only the submitting core life can read the log. The core records each complete entry in the invocation's log streams as it arrives, so Run History and `runs logs` show a long build or Card call as it progresses. A partial trailing entry waits for its remaining bytes. When more bytes are waiting, the core asks again at once; otherwise it polls every 750 ms. The cursor lives in the running invocation, so it survives disconnects and listener restarts within one core life and never repeats an entry. The completed result's `logs.jsonl` stays authoritative: its prefix must match the recorded entries byte for byte (checked by length and SHA-256), and only the entries after it are added. A cancelled, failed or interrupted job keeps its workspace until release, and the core collects the rest of its log with the same bounded, interruptible requests before it records the outcome.

For hard limits, pass `--cgroup-root` naming an owned, delegated cgroup v2 subtree. The worker enables CPU and memory controllers and sets the requested limits before the Card starts. Limits apply to the Card process and its children. Without delegation, write zero for both limits. The namespace launcher dies with its supervisor and tears down the job's PID namespace. A restarted listener also enforces cancellation against supervisors by verified boot ID, process start time and pidfd.

## Derivations and build outputs

The shared-store transport is used for local work. For a remote build the core publishes the evaluated `.drv` closure before submission. The job identifies the derivation and the expected output as Nix store paths. Its fingerprint binds the job to a revision of the cache settings. Hostd injects the pull list and push target into the authenticated submission after checking authorization and that revision. The worker holds that snapshot in memory and passes it to the invocation supervisor through a sealed descriptor. A listener that lost a queued job's snapshot asks the submitting core life to resubmit it before starting the job.

The listener appends the worker's own pull caches from `STATE/settings.json` to the host's pull list when it starts the job's supervisor, so they apply to jobs that start after a change. It passes the Nix `max-jobs` and `cores` it started with, if set, on the supervisor's command line; the invocation sets them over the machine's `nix.conf` for its builds. The worker substitutes the derivation and the available prerequisites with signature checks enabled. It walks the missing part of each closure (the `.drv`'s, then the output's) and takes every path a pull cache serves from that cache directly; only a path no pull cache serves comes from the push target. For the host-managed cache, the worker reads and writes standard Nix cache objects over a stream on the existing mutual TLS connection. For other caches, Nix uses the configured substituters and the worker publishes through the selected client. The worker builds outputs it could not substitute. While it fetches path by path, the job's activity is `Substituting` with the path and `done/total · bytes / bytes`.

The Card's resolved `pushOutputs` setting controls publication of the realised output closure, which happens after the Card's process has exited, never before it starts. The worker leaves out every path a pull cache serves with a trusted signature and every path the target already has, and uploads the rest path by path, references first, as activity `Pushing` with the same progress. A failed or unreachable target is recorded, logged as a build line, noted by the core as a `push_failed` warning in the run journal and never changes the Card's outcome. Pulling is as forgiving: the worker runs Nix with `fallback`, so a pull cache that cannot answer is logged and skipped for the job rather than failing it. The completed result reports build or cache provenance and output publishing (paths and bytes sent, paths skipped, the error) separately from invocation data.

## Host authorization and following a run

hostd stores registrations, worker public pins and project capabilities in `host-core/execution.sqlite` (0600), its Ed25519 private key and self-signed certificate in `host-core/execution-identity.pem` (0600), and a scope HMAC key in `host-core/execution-scope-key` (0600). List/put/remove replies contain only public information. OpenSSL 3, already present in the dependency closure, supplies TLS; the macOS app bundles its two runtime libraries beside hostd.

Hostd authorizes the project through a separate `BrokerRequest` envelope.
The [remote worker boundary](security.md#remote-worker-boundary) defines peer
trust and project scope. The broker capability lives in the project's
control share with mode 0600 on native Linux, or mode 0644 under the
host-private virtiofs parent on the Mac.

## Worker selection

The core considers available workers whose sandbox, grants and resource limits support the Card. The local worker comes first, followed by authorized external workers in registration order. Without a probe, the core takes the first free eligible worker and preserves an eligible resident worker’s affinity. See [target systems](#target-systems) for native and emulated execution.

A Code Card can return a `probe` derivation from `default.nix`. For example, with `pkgs` imported from the Card's locked package set:

```nix
{
  inherit run;
  probe = pkgs.writeShellScriptBin "probe" ''
    printf '%s\n' 1
  '';
}
```

Tsugumi builds the probe for each eligible worker's native system without emulation and calls its executable at `bin/probe`. It evaluates `default.nix` with the usual `params` and Nix `inputs`. The executable receives the Card's sandbox, limits and grants, fresh scratch, and `TSUGUMI_WORKER_KIND` set to `local` or `remote`. It has no invocation inputs, artifacts, resident state or Card secrets. The executable has up to five seconds, or the Card's timeout if shorter. Probe evaluations and builds use up to 60 seconds for the build budget, also capped by the Card's build timeout. Its output (standard output and error, one stream) is limited to 8 KiB.

The last line of its output must contain a finite decimal number. Zero and negative values rule the worker out. A failed build, nonzero exit, timeout, invalid number or excessive output is recorded as a failed probe. The core allows a separate period for a remote probe's cache transfer and admission. It selects the highest positive score and waits for that worker if it is busy. Ties use the same local-first order. If no eligible worker scores above zero, selection fails with the measurement diagnostics: the Card is not started, and its failure carries `stage` = `worker_selection` and, for each worker, `worker:<id>` = `unavailable`, `not_positive`, `timeout` or `failed` (with `timeout:<id>`, the limit in seconds, when it is known). The UIs say this in plain words in the Card's log and the invocation's history, where each measurement keeps its failure kind, limit and output.

Hostd reuses measurements for the same probe output and execution settings across authorized projects. It keeps up to 128 results per worker in memory. A registration change or worker restart invalidates that worker's measurements. The time, output and cache limits are fixed.

While a remote job runs, its status says what it does: waiting for the worker's Nix store while another invocation holds it, preparing, building or substituting a derivation (named), or running the Card. The worker also samples the CPU time of the job's process tree (Nix builds and the Card) every two seconds and lists the derivations it builds. The core carries these to the invocation's live summary and to the active invocations of `RunStatus`, beside the time since the invocation last wrote output or build log, so a long silent build can be told from a stuck or waiting one. A local invocation reports CPU time the same way, its Nix builds included as a remote job's are. The core's Nix build client samples, every second while it builds and once more when the build is done, the CPU time of its own process tree (a store it opens itself runs the builders below it) and of the nix-daemon processes serving its connections, with the builders below them and those they reaped; nix-daemon names such a process by its client's process ID (in its second argument, or as `nix-daemon.worker-<pid>` in newer Nix). A derivation another invocation builds meanwhile runs below that invocation's daemon process and counts there. The client's own start-up is not counted. It sees the daemon's processes itself from the same PID namespace with a readable `/proc`, as on a Linux host. In the shared VM a project's core has a PID namespace of its own, so `tsugumi-vmd`, which sees the daemon, answers the client the same count for the daemon processes serving it ([Architecture](Architecture.md)). On a Mac host without the VM the daemon's processes belong to root, so there the build's CPU time is not reported. (Nix's own build results carry a CPU time field, but Nix 2.34 and 2.36 leave it empty even for builds in cgroups.) The build's CPU time belongs to the batch's first invocation, which also carries its preparation stages. While the Card's call runs, the core then samples the Card worker's process tree (the worker, its children and what they reaped) every two seconds and once more when the call answers, counted from when the call began, since a worker started ahead or kept resident has run before. A fast pack calls the Card inside the core's own process and reports no CPU time for its call. The last value stays with the invocation in history (`InvocationSummary.cpu_ms`), local or remote, so a finished invocation shows the CPU time it used in the Card's hover details, the log's pipeline line, Run Info and its run record. The worker's Nix build log reaches the invocation's build log as it is written.

Card canvases display the current filename and transferred/total byte counts while input or output files move, including local copies and the remote worker's staging. These are transport observations, carried by the core and rendered by the UIs. Large-file progress notifications are emitted at most every 100 ms, plus the beginning and completion of a file.

Remote Cards require filesystem and PID isolation and start a fresh worker for each invocation. Remote workers support network grants. Host path and GUI grants require a suitable local worker.

See [interrupted runs](RunStorage.md#interrupted-runs) for startup behavior. A worker or broker outage does not delay local recovery.

Within the same core life, restoring the same registration and journal resumes polling and result transfer, without a second submission. The remote worker contract requires a fresh random `core_life_id` for each core. Each core creates this identity and sends it in scoped requests. Regular status and log exchanges detect a lost connection; there is no separate heartbeat or silence expiry. Another core life cannot resume the job. Independent Card-call/build timeouts and explicit cancellation still apply.

## Target systems

- A Card lists the Nix systems it works on in **Systems** in its settings (`"systems": ["aarch64-linux", "x86_64-linux"]` in `_card.json`). A worker whose native system is in the list builds and runs the Card natively; any other worker builds and runs the first entry under an emulator, for example `["x86_64-linux"]` for software with no aarch64 build. The worker is chosen without regard to the list. nixpkgs is imported for that system, and the kernel runs its binaries with an emulator registered with binfmt_misc.
  - **Mac (the shared VM):** Rosetta when it is installed on the Mac. qemu (`qemu-user` from cache.nixos.org) only when asked for with `TSUGUMI_VM_X86=qemu`, for a Mac without Rosetta or to try the qemu path. The app never installs Rosetta itself.
  - **Linux:** whatever the system registered, such as `boot.binfmt.emulatedSystems` on NixOS (with Nix's `extra-platforms`).
- Each invocation records the emulator that ran it (its path; qemu's includes its version), shown in the Info tab of the Card's run history. Rosetta and qemu need not give the same numbers, so the record tells them apart.
- Every Code Card is a `lib/main.so` loaded by a worker, so the Card's build plan also holds a worker for the selected system, built by the core from its own nixpkgs pin, and the kernel runs it under the emulator. The worker and Card use the same target runtime libraries. It is built once per nixpkgs and system, under emulation the first time, and reused from the store after that by every such Card, whichever emulator built it.
- Without an emulator, the run stops before building, saying what is missing.

## Card secrets

A Card's [secrets](security.md#card-secrets) reach a remote worker for one job only, and only in memory. The job's `JobSpec` lists the names the Card declares, never a value, so the fingerprint, the journal and the copy of the spec in the job's scratch hold names alone.

1. Before uploading anything, the core checks with hostd that every declared name is set for the project; a missing one fails the invocation, naming it, and nothing is submitted.
2. The listener starts the job's supervisor with one end of a pipe; only the listener holds the other. The supervisor passes it to the invocation unread, and nothing the invocation starts inherits it.
3. Once the invocation has realised the Card's environment and only the Card's process remains to start, it creates an empty `work/secrets-wanted` marker, and status replies carry `secrets_wanted`.
4. The core then reads the values from hostd and sends them in a `JobSecretsRequest` for that job: through hostd's broker, which fills in the project's scope, over the mutually authenticated TLS connection. The listener accepts it once, only from the submitting core life, for the job's fingerprint and exactly its declared names, writes the values into the pipe and closes it. The listener keeps no copy, and the RPC traces of hostd and the core redact the values.
5. The invocation sets each value as an environment variable of the same name in the Card's process, as locally, and drops its own copy after launch.

A listener restart before step 4 closes the pipe: the invocation fails, saying the worker restarted before the Card started, and nothing sends the values again; a person runs the Card again. A delivery whose outcome the core cannot confirm is not repeated either; if the values did not arrive, the invocation fails after five minutes. A restart after the values arrived does not affect the job: its supervisor and invocation continue as usual.

## Journal and scratch lifetime

State defaults to `$XDG_STATE_HOME/tsugumi/remote-execution` or `$HOME/.local/state/tsugumi/remote-execution`; override with `--state-dir` or `TSUGUMI_REMOTE_STATE_DIR`.

- `settings.json` (0600) holds the [worker settings](../RemoteExecution.md#worker-settings): FlatBuffers JSON text checked against `protocol/worker_settings.fbs`, created with the defaults at the first start and replaced atomically by the dashboard. A listener refuses to start with a file that does not match the schema or holds invalid values.
- `worker.lock` makes the state directory one listener's: a second `tsugumi-workerd` on it refuses to start. The dashboard's restart starts the same program again with the same command line (`execv`), which takes the lock again and adopts the running supervisors.
- `dashboard-identity.pem` (0600) is the dashboard's ECDSA P-256 key and self-signed certificate. `admin-password` (0600) holds the administrator's salted scrypt hash and is replaced atomically. See [the dashboard](#dashboard).
- `server_id` and `projects/SCOPE/instance_id` are stable identities. Replacing them makes the core reject reconnecting against a different journal. Core life IDs are separate and never reused across core starts.
- `store/nix/store` and the private store's Nix database persist across jobs. Only derivations, sources and built definitions enter this store. It is shared by this worker's jobs, which realise and build in it concurrently. Only opening the store is serialized, by `store/realise.lock`: Nix creates a missing database and runs schema migrations with only a shared lock of its own. After that Nix's own locking suffices: SQLite guards the database, and each import, substitution or build takes the path's lock and checks validity again under it, so jobs that need the same output produce it once while unrelated builds overlap. Nothing collects garbage in this store, and job removal deletes only job scratch.
- `projects/SCOPE/jobs/JOB/` is job scratch: chunk staging, verified invocation input blobs, exact `spec.fb`, private workspace, result blobs and logs. Inputs remain isolated by project and job. Stopping the sandbox releases execution resources but keeps the workspace, including partial results and logs. Accepted-job payloads stay until the same living core has durably saved the result or failure and acknowledges payload release; an interrupted core life does not send that acknowledgement. The same living core can fetch them again within its core life; there is no automatic result reuse. Never-submitted uploads remain staged across pauses in the transfer.
- `state` is atomically replaced and fsynced. It records state, fingerprint, deadline, submitting core life, supervisor identity and the result reference or bounded diagnostic needed within that life. Locks serialize submission, cancellation, completion and release. A cancellation that wins before terminal commit suppresses the result.
- The core records its payload-deletion decision in a durable acknowledgement outbox. Startup interruption records that payloads must be kept. Other completions send `ReleaseRequest` after the local completion is durable. It retries after disconnects and restarts. On release the worker first commits a `Released` tombstone, then removes **all** job scratch, including inputs, outputs and logs. A crash during deletion is finished at the next inspection. Only identity/fingerprint tombstones and lock files remain to reject delayed submissions; they contain no spec, result reference, logs or run payload. Release is idempotent.

Cancellation and core startup cleanup free execution slots without deleting accepted-job payloads or importing old results. The same applies when the supervisor is lost. Removing host run history does not itself delete remote jobs; see [Data left on a worker](../RemoteExecution.md#data-left-on-a-worker).

[Inspect or remove job data on the worker](../RemoteExecution.md#data-left-on-a-worker). See [repository tests](../../tests/README.md) for integration checks.
