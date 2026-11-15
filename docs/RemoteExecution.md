# Run a Card on a remote worker

The host's `tsugumid` evaluates a Card's Nix definition for the worker's reported system. The remote worker receives that derivation's input closure, realises it by substitution or building, runs the Card in a sandbox and returns typed outputs, files, logs, displays and provenance. Workers never evaluate Card definitions.

The [worker contract](../protocol/remote_worker.fbs) defines framed FlatBuffers requests and transfer limits; implementation is in [workerd/](../workerd/). The broker lives in host-side `tsugumi-agentd`. The Mac's Virtualization.framework adapter only copies bytes from vsock port 1027 to agentd; the Web host gives project daemons agentd's private Unix endpoint.

## Install and register

The execution host needs Linux, Nix, and unprivileged user, mount and PID namespaces. From the repository root:

```sh
nix build .#remote-worker --out-link "$HOME/.tsugumi-remote-worker"
"$HOME/.tsugumi-remote-worker/bin/tsugumi-remote-worker" --host 0.0.0.0
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

Use `--max-jobs` to change the default cap of eight jobs across all project scopes. Additional accepted jobs wait durably. The timeout starts at acceptance, including queueing, building and running. Queued deadlines survive restarts. A running invocation is never automatically rerun: a listener restart can adopt a surviving supervisor through its lock only while the submitting core life remains connected; a lost supervisor becomes interrupted. Cancellation of an unknown job creates a fingerprint tombstone, preventing a late submission from starting it.

The core supplies each job's recording budget in `JobSpec.log_limit_bytes` (currently 8 MiB). Local and remote runs apply that same budget to combined build/stdout/stderr JSONL entries. Entries exceeding the remaining budget are omitted; a journal notice records the truncation outside the budget. The worker accepts budgets from 1 byte to 16 MiB, with 128 bytes reserved for the notice. This is a core recording policy, not a listener setting.

For hard limits, pass `--cgroup-root` naming an owned, delegated cgroup v2 subtree. The worker enables CPU and memory controllers and verifies per-job limits before launching. Limits cover the build and invocation. Without it, omit both CPU and memory limits. The namespace launcher dies with its supervisor and tears down the job's PID namespace. A restarted listener also enforces cancellation/deadlines against supervisors by verified boot ID, process start time and pidfd.

## Host authorization and following a run

agentd stores registrations, worker public pins and project capabilities in `agent-core/execution.sqlite` (0600), its Ed25519 private key and self-signed certificate in `agent-core/execution-identity.pem` (0600), and a scope HMAC key in `agent-core/execution-scope-key` (0600). List/put/remove replies contain only public information. OpenSSL 3, already present in the dependency closure, supplies TLS; the macOS app bundles its two runtime libraries beside agentd.

agentd derives each project scope from the host directory's device, inode and birth time. A filesystem without birth time gets no remote capability. A random local broker capability is written only to that project's control share, not returned over RPC (0600 on Linux; 0644 inside the Mac's host-private virtiofs parent, since the guest has a different uid). It survives agentd/UI restarts. Explicit project stop revokes it durably. Replacing/removing a registration invalidates capabilities issued against the previous registry before contacting any worker; settings reauthorize running projects against the new registry. Scope identity remains stable across that reauthorization and directory renames; copied/replaced directories get different scopes.

The local capability travels in a separate `BrokerRequest` envelope. [Security](security.md#remote-worker-boundary) describes scope and peer checks.

Choose the service in Card settings. `_card.json` stores `"executionService": "compute"` and optionally `"executionResources": { "cpuMillis": 1000, "memoryBytes": 4294967296 }`. Remote Cards require filesystem isolation and fresh workers; host-folder grants and resident workers are unavailable. The execution host receives the invocation's inputs, so register hosts trusted with those inputs.

A run lives only while the `tsugumid` process that started it lives, on local and remote workers alike. On core startup, unfinished remote invocations become `failed` with failure kind `interrupted` and diagnostic `tsugumid restarted while the run was active`, exactly like local invocations. Nothing is rerun, no old result or log is fetched, and downstream Cards do not resume. Startup queues best-effort cancellation and release even if the old job already succeeded. A worker or broker outage does not delay recording the interruption.

The invocation's **Run History → Info** shows its service, job ID, architecture, limits and last confirmed state. **Disconnected** means the outcome is unconfirmed. Within the same core life, restoring the same registration and journal resumes polling and result transfer, without a second submission. Remote worker contract 5 requires a fresh random `core_life_id` for each core. Each core creates this identity, sends it in scoped requests and renews it with independent probes once per second for each used target. Another core life cannot resume the job.

The listener and supervisor cancel accepted jobs after `kCoreDisconnectTimeout` (45 seconds) without contact from their submitting core. This is the existing 30-second control exchange limit plus the 15-second kill fallback, allowing a stalled exchange and another probe before expiry. The assumption is that a core silent for that interval must forfeit its running jobs. Core crashes and temporary network cuts are real failure paths exercised by the core fault tests and remote integration tests. A timeout cannot distinguish death from a longer partition: it bounds orphan execution, but also cancels a live core's jobs after a long outage. Reconnecting after expiry cancels the old jobs before renewing contact; a cancelled job never resumes. The listener retains its existing additional 15-second verified-kill fallback for an unresponsive supervisor. Lease deadlines and job ownership survive listener restarts, and a supervisor checks them even while the listener is absent.

## Journal and scratch lifetime

State defaults to `$XDG_STATE_HOME/tsugumi/remote-execution` or `$HOME/.local/state/tsugumi/remote-execution`; override with `--state-dir` or `TSUGUMI_REMOTE_STATE_DIR`.

- `server_id` and `projects/SCOPE/instance_id` are stable identities. Replacing them makes the core reject reconnecting against a different journal. Core life IDs are separate and never reused across core starts.
- `store/nix/store` and the private store's Nix database persist across jobs. Only derivations, sources and built definitions enter this store. It is shared by this worker's jobs.
- `projects/SCOPE/jobs/JOB/` is job scratch: chunk staging, verified input/archive blobs, exact `spec.fb`, private workspace, result blobs and logs. Inputs remain isolated by project and job. The workspace is removed after the sandbox exits and results are imported. Accepted-job blobs stay until the host has durably saved the result or failure. The same living core can fetch them again while its lease remains valid; there is no automatic result reuse. Never-submitted uploads expire after one hour without an upload chunk.
- `state` is atomically replaced and fsynced. It records state, fingerprint, deadline, submitting core life, supervisor identity and the result reference or bounded diagnostic needed within that life. Locks serialize submission, cancellation, completion and release. A cancellation that wins before terminal commit suppresses the result.
- The core's completion transaction (including startup interruption) queues cancellation and `ReleaseRequest` in a small acknowledgement outbox. It retries after disconnects and restarts. On release the worker first commits a `Released` tombstone, then removes **all** job scratch, including inputs, outputs and logs. A crash during deletion is finished at the next inspection. Only identity/fingerprint tombstones and lock files remain to reject delayed submissions; they contain no spec, result reference, logs or run payload. Release is idempotent.

A core that never returns loses its accepted jobs after the lease expires. Cancellation follows the existing policy: the supervisor removes its workspace after exit, while verified blobs remain until release; expiry adds no data deletion. Startup cleanup releases old jobs without importing results. These existing workspace and release paths automatically delete run data, a tension with the Philosophy that is recorded here without changing that policy. Stop the worker and explicitly remove abandoned scratch when no core can release it. Removing host run history does not itself delete remote jobs. Host-side run retention and artifact choices are unchanged; this remote scratch is a transfer workspace, not a second notebook or result cache.

For integration checks, use [Repository tests](../tests/README.md).
