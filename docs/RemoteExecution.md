# Run a Card on an execution daemon

Register a Linux execution daemon in Tsugumi to run Code Cards on another machine. The project daemon evaluates the Card's Nix definition. The execution daemon builds it in a private Nix store, runs the isolated worker and returns typed outputs, files, logs, displays and execution provenance.

## Start a daemon

The execution host needs Nix and unprivileged Linux user and PID namespaces. Build and run the daemon from the repository root.

```sh
nix build .#remote-daemon --out-link "$HOME/.tsugumi-remote-daemon"
mkdir -p "$HOME/.config/tsugumi"
umask 077
openssl rand -hex 32 > "$HOME/.config/tsugumi/remote-token"
"$HOME/.tsugumi-remote-daemon/bin/tsugumi-remote-daemon" \
  --token-file "$HOME/.config/tsugumi/remote-token"
```

The daemon runs in the foreground and listens on `http://127.0.0.1:8765` by default. The default endpoint works when the Tsugumi host and the execution daemon can reach the same loopback interface. A connection across machines uses HTTPS. Choose the listening address and port with `--host` and `--port`, and supply a certificate and private key with `--tls-cert` and `--tls-key`. The Tsugumi host verifies the certificate for the registered endpoint. A tunnel that terminates on loopback can also carry the loopback connection.

The daemon accepts up to eight active jobs by default. Set `--max-jobs` on the execution host to change that limit. Additional submissions remain in the daemon's durable queue. The run timeout begins when the daemon first accepts the submission and includes time waiting for a slot, building and running. Restart the daemon with the same state directory to continue the queue and adopt surviving jobs.

To offer hard CPU and memory limits, pass `--cgroup-root` with a private cgroup v2 subtree delegated to the daemon's user and with the CPU and memory controllers enabled. A job that requests limits gets its own child cgroup. The daemon verifies the configured limits before running that job.

## Register the endpoint in Tsugumi

1. Open **Settings → Execution service** in the macOS app or Web UI. Add a name, a service ID such as `compute`, the daemon's **Endpoint** and its access token. Service IDs use lowercase ASCII letters, digits and hyphens and start with a letter. The built-in service uses the ID `local`.
2. Test the connection and save the registration. The daemon reports its identity, Linux architectures, isolation, resource limits, worker contract and concurrent job limit. Build the daemon from a compatible Tsugumi revision if the worker contract differs.
3. In the Code Card's settings, choose the registered service and use its native architecture or a supported explicit architecture. Enter CPU and memory limits when the daemon advertises support. The limits apply to the entire job, including its build. A daemon without that capability can run Cards that omit resource limits.
4. Run the Card. In **Card settings → Run History → Info**, select the invocation to see the service ID, job ID, architecture, limits and last confirmed state. **Packages** shows the recorded Nix environment when available.

The equivalent fields in `_card.json` are `"executionService": "compute"` and the optional `"executionResources": { "cpuMillis": 1000, "memoryBytes": 4294967296 }`. Omitting the service selects local execution. An unavailable service remains selected in a shared project until the user registers it or chooses another one.

The endpoint and access token stay with the Tsugumi host configuration. Project files carry the service ID and optional resource request. An Agent save that selects or changes an external service requires approval. The execution host receives the build inputs, values and files needed by the invocation, so register hosts you trust with those inputs.

## Follow a remote run

**Queued at the execution service** means the daemon has accepted the job and is waiting for an execution slot. **Disconnected (outcome unconfirmed)** means Tsugumi cannot confirm the current state. Restore the same endpoint and preserve both the daemon's state directory and the Tsugumi host's registration so the project daemon can check the existing job.

A cancellation stays in **Waiting for cancellation** until the daemon confirms the outcome. Opening the original project after the project daemon restarts resumes checking submitted jobs and collecting their results. In **Run History → Info**, **Cancel pending jobs in this run** requests cancellation of jobs pending from an interrupted run. Run the interrupted Workflow again to continue through its downstream Cards.

Renaming a project folder on the same filesystem preserves its job scope. A copy gets a new scope. Remote execution requires a filesystem that reports the project folder's creation time.

Remote invocations use fresh workers with filesystem isolation. Logs and displays return with the result. Use local execution when a Card needs host-folder grants or a resident worker. Progress reporting during long jobs is tracked in [Remote execution follow-ups](https://github.com/gen740/Tsugumi/issues/221).

## Retention on the execution host

The default state directory is `$XDG_STATE_HOME/tsugumi/remote-execution` or `$HOME/.local/state/tsugumi/remote-execution`. Override it with `TSUGUMI_REMOTE_STATE_DIR` when starting the daemon. Keep it private to the daemon's user. It contains the daemon identity, project-scoped jobs, build and input blobs, and result files, logs and displays. There is no automatic expiry. Deleting the Workflow's run history leaves this state on the execution host.

Archiving incompatible local history also leaves existing jobs on the execution host. The new history cannot reconnect to them. Confirm active jobs have finished or been cancelled before archiving when possible.

Each job builds in its own Nix store under the daemon's work directory. The supervisor attempts to remove this workspace when the job ends. A forced stop or failed cleanup can leave files behind.

To remove a daemon's data, stop submitting jobs, wait for its active jobs to finish or cancel them, then stop the daemon and remove its state directory. This removes jobs and blobs for every project using that daemon state.
