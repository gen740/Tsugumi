# Run a Card on an execution daemon

Register a Linux execution daemon in Tsugumi to run Code Cards on another machine. The project daemon evaluates the Card's Nix definition. The remote worker realises the derivation in a private Nix store by substituting or building, runs the isolated Card worker and returns typed outputs, files, logs, displays and execution provenance.

## Worker contract

Only the host-side `tsugumid` evaluates a Card's Nix definition. Assignment resolves an `ExecutionTarget` (worker kind and Nix system) before evaluating the Card and its connected exports or external sources. The remote daemon's `probe` already reports `native_system` and `systems`; the core uses the fresh probe's native system unless the Card selects an explicitly supported system. An unknown system or an evaluation failure for that system stops assignment with a diagnostic before any closure or invocation is uploaded. Evaluation caches include the target system, and evaluating for another system never builds an output on the host.

The common contract is `WorkerDerivation` in `core/src/worker_derivation.hh`: the `.drv` path, expected manifest output and target system, with the derivation's input closure made available. The invocation separately supplies typed inputs, parameters and call identity. Sharing the derivation is mandatory whenever an invocation is assigned to a remote worker, and it is evaluated for that worker's system; there is no per-Card switch for sharing it. Workers never evaluate Card definitions.

- A **native worker** (`tsugumi-worker`, service ID `local`) shares the host's `/nix/store` paths. Host-side preparation realises the derivation through nix-daemon in that store, reads the manifest and starts the native worker with the resulting paths. The native worker runs them directly.
- A **remote worker** (`tsugumi-remote-worker`, supervised by the Python daemon in `remote/`) has its own store. It receives the derivation contract and invocation, verifies the system and expected output, realises the derivation by substituting or building in that store, and runs its output.

`DerivationClosureTransport` is the narrow availability interface. `SharedStoreDerivationTransport` requires no transfer for native workers. `ArchiveDerivationTransport` in `core/src/derivation_transport.cc` exports the `.drv` input closure with `computeFSClosure` and `exportPaths`; the existing content-addressed blob upload carries that archive to the remote worker, which imports it before realisation. A binary-cache implementation can replace this transport without changing the scheduler or evaluator. Built outputs are not pushed by this transport.

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

## Planned

This contract prepares a binary-cache transport, but this change adds no cache server or output uploads. The planned shared cache service will live in the core's code, separately from each per-project `tsugumid`, and will be off when only native workers are used. What it keeps for reuse is derivations and built outputs only, never invocation results.

Run data (a Card's input and output values, files and directories) remain managed by `tsugumid`. The cache must not extend the existing retention rules: the last run's inputs are kept by default for partial runs, and an output is kept only when the Card explicitly declares it an artifact. This follows the project's [philosophy](Philosophy.md): people give results their meaning, so deciding which result is worth keeping belongs to a person, explicitly, rather than to a machine accumulating results on its own. It is also a practical boundary: run data grow with every combination of inputs and parameters, and a Card need not be deterministic. Caching invocation results would both cause unbounded growth and risk returning wrong results.

Run data always travel on `tsugumid`'s own channel with each invocation, separately from the derivation transport, whatever their size. Data that a user wants shared the way builds are goes into a Nix definition by the user's choice; it is then a derivation output and is cached like any other. Stored run data are deleted only by the user, in the UI: `tsugumid` may suggest a deletion (past runs, for example) but never deletes on its own, in the way backups are handled. The run history is planned to show, for each run, whether its data are still kept. These rules do not yet apply to the current remote daemon's journal and blobs described above, which stay on the remote host until someone removes them; how to handle them is undecided.

Derivation sharing remains mandatory for every remote assignment. Only pushing a Card's built output will be a per-Card setting, on by default. A remote worker is trusted once the user registers it, so it may push built outputs to the planned cache. Cards built with machine-specific optimisation must turn pushing off. Detecting such optimisation in a derivation to default pushing off or warn is planned only; this change implements neither detection nor warnings. Per-project nix-daemon ownership and build attribution are unchanged.
