# Run a Card on a remote worker

Register a Linux worker to build and run Cards on that machine. Review the [data boundary](design/security.md#remote-worker-boundary) before sending inputs.

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

All worker connections, including its private Unix socket `STATE/worker.sock`, use TLS 1.3. `--socket`, `--host` (an IPv4 or IPv6 literal) and `--port` (default 8765) configure listeners. The Unix listener remains for local integration tests and cannot be reached over the network; it has exactly the same mutual authentication as TCP. There is no plaintext listener. The default TCP bind is `127.0.0.1`; use an explicit routable bind for another machine. Review the [authentication boundary](design/security.md#remote-worker-boundary) before authorizing a host.

## See which worker runs a Card

Tsugumi chooses among the local environment and authorized external workers. A Card can supply a probe to rank eligible workers. [Worker selection](design/RemoteWorker.md#worker-selection) describes eligibility, scoring and time limits.

In **Settings → Execution service**, use **Measure again** beside a worker or **Measure all again** to invalidate and rerun its known probes immediately. Settings show scores and failures when the measurements finish. A probe runs through the project that supplied its cached measurement. If the project is unavailable or the Card's probe or permissions have changed, settings report the failure. The invocation's history shows the scores and bounded probe output used in its selection even with log and value recording turned off.

The invocation's **Run History → Info** shows its service, job ID, architecture, limits and last confirmed state. **Disconnected** means the outcome is unconfirmed.

## Data left on a worker

A person who can log into the worker machine sees and removes what is left there with `tsugumi-workerd jobs`. It reads the same state directory as the listener (`--state-dir` or the [default state directory](design/RemoteWorker.md#journal-and-scratch-lifetime)) and works whether or not the listener is running. Nothing is removed automatically, and there is no "remove everything old" mode: the command shows, the person decides.

```sh
tsugumi-workerd jobs list            # jobs that still hold data
tsugumi-workerd jobs list --all      # also released tombstones
tsugumi-workerd jobs list --json     # full IDs, for scripts
tsugumi-workerd jobs remove PROJECT/JOB...
```

`list` prints one row per job: project scope, job ID, state, size on disk, last written time (UTC), the core life that submitted it, when that core was last seen, and why the data is still there, such as `inputs staged, never submitted`, `not released: the result was never collected` or `running (supervisor pid N)`. The worker does not record when a core was last seen, so that column says `unknown`; whether the run's `tsugumid` still lives is known only on its host. The text form shortens IDs to 12 characters; `--json` gives them in full.

`remove` takes `PROJECT/JOB`, each an unambiguous prefix of at least 8 characters, and removes only the jobs named. For each job it takes the job's supervisor lock and journal lock, as the listener does, then commits a `Released` tombstone and erases the job's scratch, exactly as a release from the core does. The tombstone keeps rejecting a late transfer or submission for that job. A job that a supervisor process is running is never removed: cancel its run first. A job the journal records as queued, running or cancelling without a supervisor is kept unless `--active` is added, once nothing should run it any more; a running listener would otherwise start a queued job, or record a supervisor-less one as interrupted.

`ProjectCacheUsage` and `RunScratchDelete` on the host still cover only local and VM data; the remote protocol has no usage listing or person-facing deletion request, and `ReleaseRequest` stays the core's transfer acknowledgement. Host-side run retention and artifact choices are unchanged.
