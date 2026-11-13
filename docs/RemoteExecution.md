# Run a Card on a remote worker

The host's `tsugumid` evaluates a Card's Nix definition for the worker's reported system. The remote worker receives that derivation's input closure, realises it by substitution or building, runs the Card in a sandbox and returns typed outputs, files, logs, displays and provenance. Workers never evaluate Card definitions.

The [worker contract](../protocol/remote_worker.fbs) defines requests and transfer limits; implementation is in [workerd/](../workerd/).

## Install and run

The execution host needs Linux, Nix, and unprivileged user, mount and PID namespaces. From the repository root:

```sh
nix build .#remote-worker --out-link "$HOME/.tsugumi-remote-worker"
mkdir -p "$HOME/.config/tsugumi"
umask 077
openssl rand -hex 32 > "$HOME/.config/tsugumi/remote-token"
"$HOME/.tsugumi-remote-worker/bin/tsugumi-remote-worker" \
  --token-file "$HOME/.config/tsugumi/remote-token"
```

The process stays in the foreground of its service terminal. It opens a private Unix socket at `STATE/worker.sock` and a loopback TCP listener at `tcp://127.0.0.1:8765`. `--socket`, `--host` (only `127.0.0.1` or `::1`) and `--port` configure them. The same token authenticates either carrier. The token must be a private, owned regular file containing 64 lowercase hex characters. State must be an absolute private directory owned by the worker's user; a singleton lock prevents two listeners from managing it.

Use `--max-jobs` to change the default cap of eight jobs across all project scopes. Additional accepted jobs wait durably. The timeout starts at acceptance, including queueing, building and running. Queued deadlines survive restarts. A running invocation is never automatically rerun: a surviving supervisor is adopted through its lock; a lost supervisor becomes interrupted. Cancellation of an unknown job creates a fingerprint tombstone, preventing a late submission from starting it.

For hard limits, pass `--cgroup-root` naming an owned, delegated cgroup v2 subtree. The worker enables CPU and memory controllers and verifies per-job limits before launching. Limits cover the build and invocation. Without it, omit both CPU and memory limits. The namespace launcher dies with its supervisor and tears down the job's PID namespace. A restarted listener also enforces cancellation/deadlines against supervisors by verified boot ID, process start time and pidfd.

## Register and follow a run

In **Settings → Execution service**, register a name, service ID, endpoint (`unix:/absolute/socket` or `tcp://127.0.0.1:8765`) and token. To reach another machine today, forward a loopback TCP port through an independently established SSH tunnel. There is no direct non-loopback listener or TLS option in this revision.

Both brokers remain on the host (Swift on macOS, TypeScript on the Web). They authorize the project, replace any supplied project scope with the host's scope, verify the pinned registration and attach the remote credential. Credentials stay out of `tsugumid` and project files. Project identity survives a rename on the same filesystem; copying the project gives it another scope. A fresh probe determines the evaluation target. An unknown system or evaluation failure stops assignment before upload.

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

## Next work and validation limits

Key authentication over TLS, or SSH as the stream carrier, is the next change. It will wrap the same framing and generated contract. This revision intentionally implements neither keys nor TLS and does not move the broker into `tsugumi-agentd` or add a cache service.

Linux tests drive the C++ listener for admission, scoping, chunk integrity, parallel submissions, tombstones and restart identity. `tests/ci-remote-worker.sh` also runs real Cards through the TypeScript broker, covering results, cancellation and listener/host restarts. Full supervisor/cgroup fault injection and exhaustive crash-boundary coverage remain work in progress; the Linux/Nix integration must be run outside a sandbox that forbids listeners and Nix builds.
