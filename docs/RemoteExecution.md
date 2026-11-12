# Run a Card on an SSH host

Use an external execution service when one Code Card needs a different Linux machine. The project daemon evaluates the Card's Nix definition. The SSH host builds its derivation and runs the Card, then returns its outputs, log and display to the Workflow. Choose the service for each Card in the macOS app or the Web UI.

## Prepare the Linux host

1. Use an x86_64 or aarch64 Linux host with Nix flakes enabled, unprivileged namespaces for Bubblewrap, and a systemd user manager with cgroup v2 CPU and memory controllers. The SSH account needs permission to launch user units and build the Card's Nix dependencies. Enable lingering for this account so its jobs can finish after the SSH session closes. An administrator can run `loginctl enable-linger ACCOUNT` if the account lacks permission.
2. On the Linux host, build the gateway from the same Tsugumi revision as the client. Run this command from the repository root.

   ```sh
   nix build .#remote-gateway --out-link "$HOME/.tsugumi-remote-gateway"
   ```

   The gateway executable is `bin/tsugumi-remote-gateway` inside that out-link. Keep the out-link at a stable path and keep the gateway state directory across upgrades. The default state directory is `$XDG_STATE_HOME/tsugumi/remote-execution`, or `$HOME/.local/state/tsugumi/remote-execution` when `XDG_STATE_HOME` is unset.
3. From the machine that runs the app or the Web server, configure an SSH alias and verify the host key using OpenSSH. The connection must work without a password prompt. On Windows, configure SSH in the WSL distribution that runs `tsugumi`. On macOS, configure SSH on the Mac. Check the connection with `ssh -o BatchMode=yes -o StrictHostKeyChecking=yes my-executor true`.

## Register a service and choose it for a Card

1. In **Settings → Execution service**, add a connection. Enter a name, a service ID such as `my-executor`, and the SSH alias in **SSH host**. IDs use lowercase ASCII letters, digits and hyphens and start with a letter. The built-in service uses the ID `local`.
2. Enter the absolute path to the executable on the Linux host in **Gateway executable**, for example `/home/alice/.tsugumi-remote-gateway/bin/tsugumi-remote-gateway`. Leave it blank if the SSH session can find `tsugumi-remote-gateway` by name. Select **Test connection** and then **Save**. The test verifies the worker contract and reports the gateway's CPU architecture. If the contract is missing or incompatible, rebuild the gateway with the client's Tsugumi revision.
3. Open the Code Card's settings and select **Execution service → SSH (my-executor)**. Keep **CPU architecture** at the execution environment's default to use the SSH host's native architecture.
4. Enter both **CPU limit (millicores)** and **Memory limit (bytes)**, for example `1000` and `4294967296` for one CPU and 4 GiB. **Run timeout (s)** defaults to one hour for external runs and can be up to seven days. The CPU, memory and time limits cap the remote systemd unit, including the build and Card processes. Jobs share the SSH host’s resources. Choose a memory limit of at least one host memory page. The host rounds it down to a whole number of pages.
5. Save and run the Card. In **Card settings → Run History → Info**, select the invocation to see the service ID, remote job ID, architecture, requested limits and last confirmed state. **Packages** shows the recorded Nix environment when available.

The equivalent fields in `_card.json` are `"executionService": "my-executor"` and `"executionResources": { "cpuMillis": 1000, "memoryBytes": 4294967296 }`. Omitting `executionService` selects the local service. Set `system` only when you need to specify an architecture advertised by the selected service.

The host keeps the registry in its app settings. Project files contain the service ID and resource request. Set up a service with that ID in each installation where you use the project. SSH keys, host aliases and gateway paths stay with the host configuration. The gateway stores each project's jobs and data in a separate directory under the SSH account. The SSH account receives the Card's build inputs and the input values and files needed by the invocation. Use a host and account you trust with those inputs.

## Follow a remote run

**Queued at the execution service** means the job is waiting to start. Jobs share the SSH host's resources once they start.

**Disconnected (outcome unconfirmed)** means Tsugumi cannot confirm the job's current state. Restore the same SSH connection and keep the gateway state directory and the app's host data so the project daemon can check the existing job. A cancellation stays in **Waiting for cancellation** until the gateway confirms the outcome. Opening the original project after the daemon restarts resumes checking submitted jobs and collecting their results. In **Run History → Info**, **Cancel pending jobs in this run** requests cancellation of any remote jobs still pending from that interrupted run. The interrupted Workflow's downstream Cards need a new run.

Renaming a project folder on the same filesystem preserves its remote jobs. A copied project has its own remote jobs. External execution requires a filesystem that reports the project folder's creation time.

SSH Direct currently runs each invocation in a fresh worker with file access restrictions enabled. Host folder grants and worker reuse are unavailable for external execution. The gateway advertises its native Linux architecture. Logs and displays return with the result, so a chart does not update live while the Card runs remotely. Local execution is available for Cards that need host folders or a resident worker.

Progress reporting during SSH jobs is tracked in [SSH execution follow-ups](https://github.com/gen740/Tsugumi/issues/221).

## Retention on the SSH host

The gateway keeps job metadata and content-addressed blobs for build inputs, input values and files, and results, including output files, logs and displays. There is no automatic expiry. Deleting a run from the Workflow's history does not remove this data from the SSH host.

Archiving incompatible local run history leaves existing SSH jobs on the gateway. The new history does not reconnect to those jobs. Whenever possible, confirm remote jobs have finished or been cancelled before archiving. The gateway's files remain available to the SSH host administrator.

Each job builds in a private Nix store under the gateway's work directory. The gateway attempts to remove it when the supervisor exits normally. A forced stop or failed cleanup can leave that workspace on disk.

To remove a gateway's data, stop submitting jobs and wait for its active jobs to finish or cancel them. On the SSH host, delete the gateway's state directory described above, or the directory set by `TSUGUMI_REMOTE_STATE_DIR`. This removes jobs and blobs for every project using that directory. Once the directory is deleted, clients can no longer reconnect to those jobs.
