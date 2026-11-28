# Run a Card on a remote worker

Register a Linux worker to build and run Cards on that machine. Review the [data boundary](design/security.md#remote-worker-boundary) before sending inputs.

## Install and register

The remote listener `tsugumi-workerd` starts `tsugumi-worker` processes for Card invocations. The execution host needs Linux, Nix, and unprivileged user, mount and PID namespaces. Run one worker per user: everything it keeps is in that user's state directory (`$XDG_STATE_HOME/tsugumi/remote-execution` or `~/.local/state/tsugumi/remote-execution`; `--state-dir` or `TSUGUMI_REMOTE_STATE_DIR` to choose another), and a second `tsugumi-workerd` on the same state directory refuses to start. From the repository root:

```sh
nix build .#workerd --out-link "$HOME/.tsugumi-workerd"
"$HOME/.tsugumi-workerd/bin/tsugumi-workerd"
```

1. Start the worker. Its startup lines print `public key: ed25519:...` and the [dashboard](#the-workers-dashboard)'s URL. It creates `STATE/identity.pem` on first use, with owner-only permissions. `--public-key` prints that same public key before starting a listener (stop a running worker first if using this command).
2. Copy the worker's public key through a trusted channel. It is the base64 DER SubjectPublicKeyInfo (SPKI), prefixed with `ed25519:`. There is no CA, SSH, trust on first use or automatic key replacement.
3. In **Settings → Execution service**, enter a name, service ID, endpoint such as `tls://worker.example:8765`, and the worker's public key. **Register** saves this public pin; it does not yet admit jobs. The same settings page shows **This host's public key** with a copy action.
4. Add Tsugumi's host public key to the worker's `STATE/authorized_keys`, then choose the saved service and **Verify and Update**. Successful mutual authentication probes the worker and makes the service available. Replacing a key requires the same explicit registration process.

For example, `authorized_keys` contains one public key per line; blank lines and `#` comments are allowed, and the dashboard shows a comment after a key as the host's name:

```text
# Tsugumi on my workstation
ed25519:PASTE_THE_COMPLETE_HOST_SPKI_HERE my-mac
```

The file must be an owned regular file, not writable by other users; symlinks and malformed keys fail closed. The worker re-reads it during each handshake. Removing a key refuses new connections immediately; an already authenticated request can finish. Private keys never leave their owner or cross Host RPC.

All host connections, including the worker's private Unix socket `STATE/worker.sock` (`--socket` moves it), use TLS 1.3 with mutual authentication. The Unix listener remains for local integration tests and cannot be reached over the network. There is no plaintext listener. Review the [authentication boundary](design/security.md#remote-worker-boundary) before authorizing a host.

### Worker settings

The worker reads `STATE/settings.json` when it starts. The first start writes it with the defaults below; change it on the [dashboard](#the-workers-dashboard) as the administrator, or by hand while the worker is stopped. It is FlatBuffers JSON text with the schema [`protocol/worker_settings.fbs`](../protocol/worker_settings.fbs), and a worker refuses to start with a file that does not match it, naming the line and column. By hand you may leave keys unquoted and end a list or object with a comma; the dashboard writes the quoted form below.

```json
{
  "name": "",
  "listen_address": "127.0.0.1",
  "port": 8765,
  "dashboard_port": 8767,
  "max_jobs": 8,
  "nix_max_jobs": 0,
  "nix_cores": 0,
  "pull_caches": []
}
```

| Setting | Meaning | Takes effect |
| --- | --- | --- |
| `name` | The name the dashboard and hosts show. Empty shows the machine's host name on the dashboard. | at once; hosts see it at their next verification or probe |
| `listen_address` | The IP address both listeners bind to. Use a routable address, or `0.0.0.0`, for other machines. | after a restart |
| `port` | The hosts' mutual TLS port. | after a restart |
| `dashboard_port` | The dashboard's HTTPS port on the same address. | after a restart |
| `max_jobs` | Jobs that run at once across all projects (1 to 65536). Accepted jobs beyond it wait durably. | for the next job that starts |
| `nix_max_jobs`, `nix_cores` | Nix `max-jobs` and `cores` for the worker's private store. 0 keeps the machine's Nix configuration. | after a restart |
| `pull_caches` | `{"url": ..., "public_key": "name:base64"}` binary caches the worker substitutes from, after the pull caches the host sends with each job, with signature checks. URLs are `https://`, `http://` or `file:///`, without credentials. | for the next job that starts |

The command line keeps only `--state-dir`, `--socket`, `--cgroup-root` and `--public-key`, and the subcommands `jobs` and `admin-password`.

## Update a worker

A worker speaks the worker contract of the Tsugumi revision it was built from. When Tsugumi moves to a revision with another contract, **Settings → Execution service** shows the worker as out of date, with its contract and this Tsugumi's, and runs do not use it: a Card that only that worker could run fails at once, before any preparation, and its failure says the worker is out of date. The worker's row shows the contract and the revision it reported when last verified; its dashboard shows the same in its header.

Rebuild the worker from the revision your Tsugumi was built from (for a release, its tag such as `v0.9.0`), then restart it:

```sh
nix build "github:gen740/Tsugumi/<rev>#workerd" --out-link "$HOME/.tsugumi-workerd"
"$HOME/.tsugumi-workerd/bin/tsugumi-workerd"
```

Stop the running listener first and start it again with the same options. Its state directory, settings, identity and `authorized_keys` stay, so the registration's public key does not change. Its startup line prints its worker contract and revision. Then choose the service in **Settings → Execution service** and **Verify and Update**: the worker's row no longer says it is out of date.

## Share Nix builds

Tsugumi evaluates the Card on the submitting host and publishes its derivation closure to the configured [push target](BinaryCaches.md). The remote worker checks signatures when it retrieves the derivation and any available substitutes for its build outputs. It builds missing outputs before running the Card. The default, `tsugumi-cached`, is started on demand by hostd. The worker reaches it through its existing TLS connection to hostd.

Each Code Card controls whether its Nix build outputs are published. **Run History → Info** shows the build or cache source and the outcome of publishing. Card inputs, outputs and files stay on the worker execution channel.

## See which worker runs a Card

Tsugumi chooses among the local environment and authorized external workers. A Card can supply a probe to rank eligible workers. [Worker selection](design/RemoteWorker.md#worker-selection) describes eligibility, scoring and time limits.

In **Settings → Execution service**, use **Measure again** beside a worker or **Measure all again** to invalidate and rerun its known probes immediately. Settings show scores and failures when the measurements finish. A probe runs through the project that supplied its cached measurement. If the project is unavailable or the Card's probe or permissions have changed, settings report the failure. The invocation's history shows the scores and bounded probe output used in its selection even with log and value recording turned off.

The invocation's **Run History → Info** shows its service, job ID, architecture, limits and last confirmed state. **Disconnected** means the outcome is unconfirmed.

## The worker's dashboard

The worker always serves a dashboard page over HTTPS on `dashboard_port` (default 8767) of its listen address, for example `https://worker.example:8767/`. At startup it prints the URL and the SHA-256 fingerprint of the page's certificate:

```text
dashboard: https://0.0.0.0:8767/ (certificate SHA-256 3A:7F:...)
```

The certificate is self-signed and kept in `STATE/dashboard-identity.pem`; it stays the same across restarts. It is an ECDSA P-256 certificate of its own, because browsers do not accept the worker's Ed25519 identity for HTTPS. A browser asks once whether to trust it: compare the fingerprint it shows with the one the worker printed. Tsugumi needs no such step: hosts learn the fingerprint over their pinned connection to the worker.

In **Settings → Execution service**, choose **Open Dashboard** beside a verified worker. The Mac app shows the page in its own view, trusting exactly that certificate, in the UI language. The Web UI opens it in a new browser tab and shows the fingerprint beside the worker.

The page refreshes every five seconds; when a refresh fails, it keeps the last data and marks it **Cannot refresh**. Anyone who reaches the port sees it without logging in, read-only: the worker's name, version, worker contract, system and uptime, its jobs across project scopes with state, disk use, last write and why each is kept, the disk used by the private Nix store and by job scratch and the free space, the settings, and the host keys in `authorized_keys` with when each last connected. It shows no secrets: no passwords, Card secrets or core identities.

### Administrator

**Log in as administrator** to remove jobs, add and remove host keys, save settings, or restart the worker. On the first start, the worker generates a random password and prints it once on standard output, on the `Initial administrator password` line. Keep that output private. Subsequent starts keep the existing password and omit the notice.

To change it, open **Change password** on the dashboard and enter the current password and the new password twice. This signs out all other sessions. You can also set or reset the password from the worker's command line. The new password takes effect at the next login without a restart.

```sh
tsugumi-workerd admin-password                 # asks twice on a terminal
printf '%s\n' "$PASSWORD" | tsugumi-workerd admin-password   # or one line on standard input
```

Use 8 to 1024 bytes. The worker keeps only a salted scrypt hash in `STATE/admin-password` (mode 0600). Wrong passwords at login and in the **Change password** form count toward the same lockout. Both checks pause for 1 minute after five consecutive failures, then for 3, 5 and 30 minutes after further failures. A successful password check resets the count. Each session lasts up to 12 hours and can end earlier on **Log out**, a dashboard password change from another session, or a worker restart.

Each change shows its result, or the worker's error, beside its button.

- **Jobs:** select jobs and remove their data after confirming. Jobs recorded as queued, running or cancelling cannot be selected, and a job whose supervisor holds its lock is never removed. Tsugumi never removes job data by itself.
- **Hosts:** add or remove a public key in `authorized_keys`. Add the first key by hand on the worker so a host can connect. Removing a key takes effect at the next handshake.
- **Settings:** the [settings](#worker-settings) above. The page marks **Restart needed** when a saved value waits for a restart.
- **Restart:** **Restart after the running jobs finish** starts no new job and restarts when none runs; **Restart now** cancels the running jobs, as a host's cancel does, and restarts at once. The worker starts its own program again with the same command line, reads `settings.json` again and keeps the queued jobs.

## Data left on a worker

The dashboard lists the job data the worker still holds. If the listener is down, log into the worker and use `tsugumi-workerd jobs`. The CLI reads the same state directory as the listener (`--state-dir` or the [default state directory](design/RemoteWorker.md#journal-and-scratch-lifetime)).

```sh
tsugumi-workerd jobs list            # jobs that still hold data
tsugumi-workerd jobs list --all      # also released tombstones
tsugumi-workerd jobs list --json     # full IDs, for scripts
tsugumi-workerd jobs remove PROJECT/JOB...
```

`list` prints one row per job: project scope, job ID, state, size on disk, last written time (UTC), the core life that submitted it, when that core was last seen, and why the data is still there, such as `inputs staged, never submitted`, `not released: the result was never collected` or `running (supervisor pid N)`. The worker does not record when a core was last seen, so that column says `unknown`; whether the run's `tsugumid` still lives is known only on its host. The text form shortens IDs to 12 characters; `--json` gives them in full.

`remove` takes `PROJECT/JOB`, each an unambiguous prefix of at least 8 characters, and removes only the jobs named. For each job it takes the job's supervisor lock and journal lock, as the listener does, then commits a `Released` tombstone and erases the job's scratch, exactly as a release from the core does. The tombstone keeps rejecting a late transfer or submission for that job. A job that a supervisor process is running is never removed: cancel its run first. A job the journal records as queued, running or cancelling without a supervisor is kept unless `--active` is added, once nothing should run it any more; a running listener would otherwise start a queued job, or record a supervisor-less one as interrupted.

The dashboard and the CLI share the worker's listing and removal code. Only the CLI's `--active` removes a job recorded as queued, running or cancelling. `ProjectCacheUsage` and `RunScratchDelete` cover local and VM data. The core uses `ReleaseRequest` to acknowledge transfers.
