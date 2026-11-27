# Remote execution and worker selection

The host's `tsugumid` evaluates a Card's Nix definition for the system the worker runs it as (the worker's native system when the Card lists it, otherwise the Card's first system). The remote worker receives that derivation's input closure, realises it by substitution or building, runs the Card in a sandbox and returns typed outputs, files, logs and provenance. What the Card sends to its views travels live through the same connection and is never written to the job's result files. Workers never evaluate Card definitions.

The [worker contract](../../protocol/remote_worker.fbs) defines framed FlatBuffers requests and transfer limits; implementation is in [workerd/](../../workerd/). The broker lives in host-side `tsugumi-hostd`. The Mac's Virtualization.framework adapter only copies bytes from vsock port 1027 to hostd; the Web host gives project daemons hostd's private Unix endpoint. Tsugumi's own [binary cache server](BinaryCacheServer.md) admits the same registered worker keys.

[Install and register a remote worker](../RemoteExecution.md#install-and-register).

## Job execution

Use `--max-jobs` to change the default cap of eight jobs across all project scopes. Additional accepted jobs wait durably. `buildTimeoutSeconds` bounds the build; `timeoutSeconds` bounds only the Card call, exactly as for local execution. A value of zero means no deadline. Waiting for a free worker and moving input, derivation or result files consume neither Card-call time nor build time. There is no aggregate job deadline. A running invocation is never automatically rerun: a listener restart can adopt a surviving supervisor through its lock within the same submitting core life; a lost supervisor becomes interrupted. Cancellation of an unknown job creates a fingerprint tombstone, preventing a late submission from starting it.

At startup the listener reads the job journals. During normal operation,
journal-lock events and supervisor process descriptors wake job scheduling.
An unreadable job journal cannot take a worker slot. Upload retries compare
overlapping bytes with staged data and charge only new bytes against the
upload budget.

The core supplies each job's recording budget in `JobSpec.log_limit_bytes`. The budget limits the recorded output. Remote workers send build, stdout and stderr in size-prefixed FlatBuffer entries and report recording truncation separately.

The log arrives while the job runs. Each status request, and each cancellation the running invocation sends, carries the core's cursor: the number of log bytes it has received. The reply returns at most 256 KiB from there and the log's current size; only the submitting core life can read the log. The core records each complete entry in the invocation's log streams as it arrives, so Run History and `runs logs` show a long build or Card call as it progresses. A partial trailing entry waits for its remaining bytes. When more bytes are waiting, the core asks again at once; otherwise it polls every 750 ms. The cursor lives in the running invocation, so it survives disconnects and listener restarts within one core life and never repeats an entry. The completed result's `logs.jsonl` stays authoritative: its prefix must match the recorded entries byte for byte (checked by length and SHA-256), and only the entries after it are added. A cancelled, failed or interrupted job keeps its workspace until release, and the core collects the rest of its log with the same bounded, interruptible requests before it records the outcome.

For hard limits, pass `--cgroup-root` naming an owned, delegated cgroup v2 subtree. The worker enables CPU and memory controllers and sets the requested limits before the Card starts. Limits apply to the Card process and its children. Without delegation, write zero for both limits. The namespace launcher dies with its supervisor and tears down the job's PID namespace. A restarted listener also enforces cancellation against supervisors by verified boot ID, process start time and pidfd.

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

Tsugumi builds the probe for each eligible worker's native system without emulation and calls its executable at `bin/probe`. It evaluates `default.nix` with the usual `params` and Nix `inputs`. The executable receives the Card's sandbox, limits and grants, fresh scratch, and `TSUGUMI_WORKER_KIND` set to `local` or `remote`. It has no invocation inputs, artifacts, resident state or Card secrets. The executable has up to five seconds, or the Card's timeout if shorter. Probe evaluations and builds use up to 60 seconds for the build budget, also capped by the Card's build timeout. Combined standard output and error are limited to 8 KiB.

The last standard-output line must contain a finite decimal number. Zero and negative values rule the worker out. A failed build, nonzero exit, timeout, invalid number or excessive output is recorded as a failed probe. The core selects the highest positive score and waits for that worker if it is busy. Ties use the same local-first order. If no eligible worker scores above zero, selection fails with the measurement diagnostics.

Hostd reuses measurements for the same probe output and execution settings across authorized projects. It keeps up to 128 results per worker in memory. A registration change or worker restart invalidates that worker's measurements. The time, output and cache limits are fixed.

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

- `server_id` and `projects/SCOPE/instance_id` are stable identities. Replacing them makes the core reject reconnecting against a different journal. Core life IDs are separate and never reused across core starts.
- `store/nix/store` and the private store's Nix database persist across jobs. Only derivations, sources and built definitions enter this store. It is shared by this worker's jobs.
- `projects/SCOPE/jobs/JOB/` is job scratch: chunk staging, verified input/archive blobs, exact `spec.fb`, private workspace, result blobs and logs. Inputs remain isolated by project and job. Stopping the sandbox releases execution resources but keeps the workspace, including partial results and logs. Accepted-job payloads stay until the same living core has durably saved the result or failure and acknowledges payload release; an interrupted core life does not send that acknowledgement. The same living core can fetch them again within its core life; there is no automatic result reuse. Never-submitted uploads remain staged across pauses in the transfer.
- `state` is atomically replaced and fsynced. It records state, fingerprint, deadline, submitting core life, supervisor identity and the result reference or bounded diagnostic needed within that life. Locks serialize submission, cancellation, completion and release. A cancellation that wins before terminal commit suppresses the result.
- The core records its payload-deletion decision in a durable acknowledgement outbox. Startup interruption records that payloads must be kept. Other completions send `ReleaseRequest` after the local completion is durable. It retries after disconnects and restarts. On release the worker first commits a `Released` tombstone, then removes **all** job scratch, including inputs, outputs and logs. A crash during deletion is finished at the next inspection. Only identity/fingerprint tombstones and lock files remain to reject delayed submissions; they contain no spec, result reference, logs or run payload. Release is idempotent.

Cancellation and core startup cleanup free execution slots without deleting accepted-job payloads or importing old results. The same applies when the supervisor is lost. Removing host run history does not itself delete remote jobs; see [Data left on a worker](../RemoteExecution.md#data-left-on-a-worker).

[Inspect or remove job data on the worker](../RemoteExecution.md#data-left-on-a-worker). See [repository tests](../../tests/README.md) for integration checks.
