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

Use `--max-jobs` to change the default cap of eight jobs across all project scopes. Additional accepted jobs wait durably. The timeout starts at acceptance, including queueing, building and running. Queued deadlines survive restarts. A running invocation is never automatically rerun: a surviving supervisor is adopted through its lock; a lost supervisor becomes interrupted. Cancellation of an unknown job creates a fingerprint tombstone, preventing a late submission from starting it.

`--log-limit-bytes N` sets a positive per-job byte limit for combined build/stdout/stderr JSONL records (default 8388608, 8 MiB); entries that exceed the remaining budget are omitted, and a final journal line reports truncation at that limit outside the budget.

For hard limits, pass `--cgroup-root` naming an owned, delegated cgroup v2 subtree. The worker enables CPU and memory controllers and verifies per-job limits before launching. Limits cover the build and invocation. Without it, omit both CPU and memory limits. The namespace launcher dies with its supervisor and tears down the job's PID namespace. A restarted listener also enforces cancellation/deadlines against supervisors by verified boot ID, process start time and pidfd.

## Host authorization and following a run

agentd stores registrations, worker public pins and project capabilities in `agent-core/execution.sqlite` (0600), its Ed25519 private key and self-signed certificate in `agent-core/execution-identity.pem` (0600), and a scope HMAC key in `agent-core/execution-scope-key` (0600). List/put/remove replies contain only public information. OpenSSL 3, already present in the dependency closure, supplies TLS; the macOS app bundles its two runtime libraries beside agentd.

agentd derives each project scope from the host directory's device, inode and birth time. A filesystem without birth time gets no remote capability. A random local broker capability is written only to that project's control share, not returned over RPC (0600 on Linux; 0644 inside the Mac's host-private virtiofs parent, since the guest has a different uid). It survives agentd/UI restarts. Explicit project stop revokes it durably. Replacing/removing a registration invalidates capabilities issued against the previous registry before contacting any worker; settings reauthorize running projects against the new registry. Scope identity remains stable across that reauthorization and directory renames; copied/replaced directories get different scopes.

The local capability travels in a separate `BrokerRequest` envelope. [Security](security.md#remote-worker-boundary) describes scope and peer checks.

Choose the service in Card settings. `_card.json` stores `"executionService": "compute"` and optionally `"executionResources": { "cpuMillis": 1000, "memoryBytes": 4294967296 }`. Remote Cards require filesystem isolation and fresh workers; host-folder grants and resident workers are unavailable. The execution host receives the invocation's inputs, so register hosts trusted with those inputs.

The invocation's **Run History → Info** shows its service, job ID, architecture, limits and last confirmed state. **Disconnected** means the outcome is unconfirmed, not failed. Restoring the same registration and journal lets the host resume polling and fetching results. Cancellation remains pending until confirmed. Opening the project after a host restart resumes existing jobs; it does not submit them a second time. Run the interrupted Workflow again to continue downstream Cards.

## Journal and scratch lifetime

State defaults to `$XDG_STATE_HOME/tsugumi/remote-execution` or `$HOME/.local/state/tsugumi/remote-execution`; override with `--state-dir` or `TSUGUMI_REMOTE_STATE_DIR`.

- `server_id` and `projects/SCOPE/instance_id` are stable identities. Replacing them makes the host reject recovery against a different journal.
- `store/nix/store` and the private store's Nix database persist across jobs. Only derivations, sources and built definitions enter this store. It is shared by this worker's jobs.
- `projects/SCOPE/jobs/JOB/` is job scratch: chunk staging, verified input/archive blobs, exact `spec.fb`, private workspace, result blobs and logs. Inputs remain isolated by project and job. The workspace is removed after the sandbox exits and results are imported. Accepted-job blobs stay until the host has durably saved the result or failure. A disconnected host can fetch them again; there is no result expiry or automatic reuse. Never-submitted uploads expire after one hour without an upload chunk.
- `state` is atomically replaced and fsynced. It records state, fingerprint, deadline, supervisor identity and the result reference or bounded diagnostic needed for recovery. Locks serialize submission, cancellation, completion and release. A cancellation that wins before terminal commit suppresses the result.
- The host's completion transaction queues `ReleaseRequest` in a small acknowledgement outbox. It retries after disconnects and restarts. On release the worker first commits a `Released` tombstone, then removes **all** job scratch, including inputs, outputs and logs. A crash during deletion is finished at the next inspection. Only identity/fingerprint tombstones and lock files remain to reject delayed submissions; they contain no spec, result reference, logs or run payload. Release is idempotent.

A host that never returns leaves its accepted job scratch available for recovery. Stop the worker and explicitly remove abandoned state if no host can recover it. Removing host run history does not itself delete remote jobs. Host-side run retention and artifact choices are unchanged; this remote scratch is a transfer workspace, not a second notebook or result cache.

For integration checks, use [Repository tests](../tests/README.md).
