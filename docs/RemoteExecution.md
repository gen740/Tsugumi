# Run a Card on a remote worker

The host's `tsugumid` evaluates a Card's Nix definition for the system the worker runs it as (the worker's native system when the Card lists it, otherwise the Card's first system). The remote worker receives that derivation's input closure, realises it by substitution or building, runs the Card in a sandbox and returns typed outputs, files, logs and provenance. What the Card sends to its views travels live through the same connection and is never written to the job's result files. Workers never evaluate Card definitions.

The [worker contract](../protocol/remote_worker.fbs) defines framed FlatBuffers requests and transfer limits; implementation is in [workerd/](../workerd/). The broker lives in host-side `tsugumi-hostd`. The Mac's Virtualization.framework adapter only copies bytes from vsock port 1027 to hostd; the Web host gives project daemons hostd's private Unix endpoint. Tsugumi's own [binary cache server](BinaryCacheServer.md) admits the same registered worker keys.

## Install and register

The remote listener `tsugumi-workerd` starts `tsugumi-worker` processes for Card invocations. The execution host needs Linux, Nix, and unprivileged user, mount and PID namespaces. From the repository root:

```sh
nix build .#workerd --out-link "$HOME/.tsugumi-workerd"
"$HOME/.tsugumi-workerd/bin/tsugumi-workerd" --host 0.0.0.0
```

1. Start the worker. Its startup line prints `public key: ed25519:...`. It creates `STATE/identity.pem` on first use, with owner-only permissions. `--public-key` prints that same public key before starting a listener (stop a running worker first if using this command).
2. Copy the worker's public key through a trusted channel. It is the base64 DER SubjectPublicKeyInfo (SPKI), prefixed with `ed25519:`. There is no CA, SSH, trust on first use or automatic key replacement.
3. In **Settings → Execution service**, enter a name, service ID, endpoint such as `tls://worker.example:8765`, and the worker's public key. **Register** saves this public pin; it does not yet admit jobs. The same settings page shows **This host's public key** with a copy action.
4. Add Tsugumi's host public key to the worker's `STATE/authorized_keys`, then choose the saved service and **Verify and Update**. Successful mutual authentication probes the worker and makes the service available. Replacing a key requires the same explicit registration process.

For example, `authorized_keys` contains one public key per line; blank lines and `#` comments are allowed:

```text
# Tsugumi on my workstation
ed25519:PASTE_THE_COMPLETE_HOST_SPKI_HERE
```

`--authorized-keys FILE` overrides the default file. It must be an owned regular file, not writable by other users; symlinks and malformed keys fail closed. The worker re-reads it during each handshake. Removing a key refuses new connections immediately; an already authenticated request can finish. Private keys never leave their owner or cross Host RPC.

All worker connections, including its private Unix socket `STATE/worker.sock`, use TLS 1.3. `--socket`, `--host` (an IPv4 or IPv6 literal) and `--port` (default 8765) configure listeners. The Unix listener remains for local integration tests and cannot be reached over the network; it has exactly the same mutual authentication as TCP. There is no plaintext listener. The default TCP bind is `127.0.0.1`; use an explicit routable bind for another machine. Review the [authentication boundary](security.md#remote-worker-boundary) before authorizing a host.

Use `--max-jobs` to change the default cap of eight jobs across all project scopes. Additional accepted jobs wait durably. `buildTimeoutSeconds` bounds the build; `timeoutSeconds` bounds only the Card call, exactly as for local execution. A value of zero means no deadline. Waiting for a free worker and moving input, derivation or result files consume neither Card-call time nor build time. There is no aggregate job deadline. A running invocation is never automatically rerun: a listener restart can adopt a surviving supervisor through its lock within the same submitting core life; a lost supervisor becomes interrupted. Cancellation of an unknown job creates a fingerprint tombstone, preventing a late submission from starting it.

The core supplies each job's recording budget in `JobSpec.log_limit_bytes`. The budget limits the recorded output. Remote workers send build, stdout and stderr in size-prefixed FlatBuffer entries and report recording truncation separately.

The log arrives while the job runs. Each status request, and each cancellation the running invocation sends, carries the core's cursor: the number of log bytes it has received. The reply returns at most 256 KiB from there and the log's current size; only the submitting core life can read the log. The core records each complete entry in the invocation's log streams as it arrives, so Run History and `runs logs` show a long build or Card call as it progresses. A partial trailing entry waits for its remaining bytes. When more bytes are waiting, the core asks again at once; otherwise it polls every 750 ms. The cursor lives in the running invocation, so it survives disconnects and listener restarts within one core life and never repeats an entry. The completed result's `logs.jsonl` stays authoritative: its prefix must match the recorded entries byte for byte (checked by length and SHA-256), and only the entries after it are added. A cancelled, failed or interrupted job keeps its workspace until release, and the core collects the rest of its log with the same bounded, interruptible requests before it records the outcome.

For hard limits, pass `--cgroup-root` naming an owned, delegated cgroup v2 subtree. The worker enables CPU and memory controllers and sets the requested limits before the Card starts. Limits apply to the Card process and its children. Without delegation, write zero for both limits. The namespace launcher dies with its supervisor and tears down the job's PID namespace. A restarted listener also enforces cancellation against supervisors by verified boot ID, process start time and pidfd.

## Host authorization and following a run

hostd stores registrations, worker public pins and project capabilities in `host-core/execution.sqlite` (0600), its Ed25519 private key and self-signed certificate in `host-core/execution-identity.pem` (0600), and a scope HMAC key in `host-core/execution-scope-key` (0600). List/put/remove replies contain only public information. OpenSSL 3, already present in the dependency closure, supplies TLS; the macOS app bundles its two runtime libraries beside hostd.

hostd derives each project scope from the host directory's device, inode and birth time. A filesystem without birth time gets no remote capability. A random local broker capability is written only to that project's control share, not returned over RPC (0600 on Linux; 0644 inside the Mac's host-private virtiofs parent, since the guest has a different uid). It survives hostd/UI restarts. Explicit project stop revokes it durably. Replacing/removing a registration invalidates capabilities issued against the previous registry before contacting any worker; settings reauthorize running projects against the new registry. Scope identity remains stable across that reauthorization and directory renames; copied/replaced directories get different scopes.

The local capability travels in a separate `BrokerRequest` envelope. [Security](security.md#remote-worker-boundary) describes scope and peer checks.

## Worker selection

The core considers workers registered for the project whose sandbox and resource limits support the Card. With equal priority, local comes first and external workers follow in registration order. A resident Card keeps its local worker when its settings allow reuse. The selected worker runs the Card natively when its own system appears in `systems`; otherwise it runs the first entry under an emulator.

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

The last standard-output line must contain a finite decimal number. Zero and negative values rule the worker out. A failed build, nonzero exit, timeout, invalid number or excessive output is recorded as a failed probe. The core selects the highest positive score and waits for that worker if it is busy. When all scores tie, registration order decides. Cards without a probe use the usual free-worker order.

Hostd reuses measurements for the same probe output and execution settings across authorized projects. It keeps up to 128 results per worker in memory. A registration change or worker restart invalidates that worker's measurements. The time, output and cache limits are fixed.

In **Settings → Execution service**, use **Measure again** beside a worker or **Measure all again** to invalidate and rerun its known probes immediately. Settings show scores and failures when the measurements finish. A probe runs through the project that supplied its cached measurement. If the project is unavailable or the Card's probe or permissions have changed, settings report the failure. Standalone packed apps use their embedded execution plan and skip probe evaluation and execution. The invocation's history shows the scores and bounded probe output used in its selection even with log and value recording turned off.

Card canvases display the current filename and transferred/total byte counts while input or output files move, including local copies and the remote worker's staging. These are transport observations, carried by the core and rendered by the UIs. Large-file progress notifications are emitted at most every 100 ms, plus the beginning and completion of a file.

Remote Cards require filesystem and PID isolation and start a fresh worker for each invocation. Remote workers support network grants. The execution host receives the invocation's inputs, so register hosts trusted with those inputs.

A run lives only while the `tsugumid` process that started it lives, on local and remote workers alike. On core startup, unfinished remote invocations become `failed` with failure kind `interrupted` and diagnostic `tsugumid restarted while the run was active`, exactly like local invocations. Nothing is rerun, no old result or log is fetched, and downstream Cards do not resume. Startup queues best-effort cancellation even if the old job already succeeded. This frees execution slots without releasing payloads: inputs, partial or complete results and logs remain on the listener. A worker or broker outage does not delay recording the interruption.

The invocation's **Run History → Info** shows its service, job ID, architecture, limits and last confirmed state. **Disconnected** means the outcome is unconfirmed. Within the same core life, restoring the same registration and journal resumes polling and result transfer, without a second submission. The remote worker contract requires a fresh random `core_life_id` for each core. Each core creates this identity and sends it in scoped requests. Regular status and log exchanges detect a lost connection; there is no separate heartbeat or silence expiry. Another core life cannot resume the job. Independent Card-call/build timeouts and explicit cancellation still apply.

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
- The core's completion transaction queues cancellation and a separate payload-deletion decision in a small acknowledgement outbox (local history format 10; portable history stays at 8). Startup interruption records that payloads must be kept. Other completions send `ReleaseRequest` after the local completion is durable. It retries after disconnects and restarts. On release the worker first commits a `Released` tombstone, then removes **all** job scratch, including inputs, outputs and logs. A crash during deletion is finished at the next inspection. Only identity/fingerprint tombstones and lock files remain to reject delayed submissions; they contain no spec, result reference, logs or run payload. Release is idempotent.

Cancellation and core startup cleanup free execution slots without deleting accepted-job payloads or importing old results. The same applies when the supervisor is lost. Removing host run history does not itself delete remote jobs; see [Data left on a worker](#data-left-on-a-worker).

### Data left on a worker

A person who can log into the worker machine sees and removes what is left there with `tsugumi-workerd jobs`. It reads the same state directory as the listener (`--state-dir` or the defaults above) and works whether or not the listener is running. Nothing is removed automatically, and there is no "remove everything old" mode: the command shows, the person decides.

```sh
tsugumi-workerd jobs list            # jobs that still hold data
tsugumi-workerd jobs list --all      # also released tombstones
tsugumi-workerd jobs list --json     # full IDs, for scripts
tsugumi-workerd jobs remove PROJECT/JOB...
```

`list` prints one row per job: project scope, job ID, state, size on disk, last written time (UTC), the core life that submitted it, when that core was last seen, and why the data is still there, such as `inputs staged, never submitted`, `not released: the result was never collected` or `running (supervisor pid N)`. The worker does not record when a core was last seen, so that column says `unknown`; whether the run's `tsugumid` still lives is known only on its host. The text form shortens IDs to 12 characters; `--json` gives them in full.

`remove` takes `PROJECT/JOB`, each an unambiguous prefix of at least 8 characters, and removes only the jobs named. For each job it takes the job's supervisor lock and journal lock, as the listener does, then commits a `Released` tombstone and erases the job's scratch, exactly as a release from the core does. The tombstone keeps rejecting a late transfer or submission for that job. A job that a supervisor process is running is never removed: cancel its run first. A job the journal records as queued, running or cancelling without a supervisor is kept unless `--active` is added, once nothing should run it any more; a running listener would otherwise start a queued job, or record a supervisor-less one as interrupted.

`ProjectCacheUsage` and `RunScratchDelete` on the host still cover only local and VM data; the remote protocol has no usage listing or person-facing deletion request, and `ReleaseRequest` stays the core's transfer acknowledgement. Host-side run retention and artifact choices are unchanged.

For integration checks, use [Repository tests](../tests/README.md).
