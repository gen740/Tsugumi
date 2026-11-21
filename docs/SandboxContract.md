# The sandbox contract

**Status:** decided by the owner on 2026-11-13 and 2026-11-21; not implemented yet.
Each section states what is decided and what the code does today. Points that are not decided yet are listed under [Open](#open).
How the current implementation isolates a Card is in [Security §4](security.md#4-card-worker--tsugumid).

## The worker builds what the Card sees

**Decided.**

- The worker builds what the Card sees.
- The contract fixes meanings, not path strings.
- Each kind of worker implements the contract with its own means: bubblewrap on Linux, and Seatbelt with the environment on native macOS.
- The environment, `HOME`, `TMPDIR` and the working directory are fixed for the lifetime of the process.

**Today.** The working directory and the environment are set again on each call ([Worker state](CardGuide.md#worker-state)). Each call also sets `HOME` to the worker's work directory.

## Directories

**Decided.**

| What | Writable | Lifetime | Isolated from |
| --- | --- | --- | --- |
| Working directory = `HOME` | yes | The process. With `reuseWorker` it survives across invocations and runs | Other processes, runs and Cards |
| TMP | yes | The path is fixed for the process; the contents are cleared for each invocation | Other runs |

**Today.**

- `HOME`, `XDG_CONFIG_HOME` and `XDG_CACHE_HOME` are writable worker directories.
- The invocation's input, output, work and temporary slots are emptied after each call ([Security §4](security.md#sandbox-true-the-default)).

## File and Directory inputs

**Decided.**

- The input value holds an absolute path. The path is read-only and valid for that invocation only.
- Inputs are hardlinked; a Directory is hardlinked file by file.
- Inputs are copied instead of hardlinked in two cases:
  - across filesystems;
  - whenever the worker cannot enforce read-only, for example with the sandbox option off. A hardlink shares the inode of the stored run data, so without read-only enforcement the Card could change those data.
- With `reuseWorker`, the inputs are in a read-only bound directory, and the worker swaps its contents for each invocation.

**Today.** The core copies the invocation's File and Directory inputs into a fixed, read-only staging area local to the worker ([Security §4](security.md#sandbox-true-the-default)).

## File and Directory outputs

**Decided.**

- The Card writes each output to the path that `CardInput` gives for that port ([Card ABI 9](CardABI.md#fixed-schemas)).
- There is no output directory.

**Today.** Outputs are written beneath `run.output_directory` (`$TSUGUMI_OUTPUT_DIR` in Bash).

## Grants

**Decided.**

- Grants are declared in the Card's definition and given by a person. They are:
  - host paths, read-only or read-write;
  - the GUI;
  - the network.
- Grants reach the Card as environment variables.

**Today.**

- `network` and `hostFolders` are set in `_card.json`. A host folder is mounted at `/host/<name>`, read-only by default ([Outside access](features.md#outside-access-experimental)).
- There is no GUI grant.
- An Agent's save that widens these waits for a person ([The Agent's permission boundary](security.md#the-agents-permission-boundary)).

## Sandbox options and the no-sandbox mode

**Decided.**

- The no-sandbox mode is kept.
- Sandbox options belong to the worker and are configurable. The options are part of the contract.
- When a Card's worker setting conflicts with what the chosen worker supports, the UI asks the person to remove that option.

**Today.**

- `sandbox` (on by default) is a Card setting ([`sandbox: false`](security.md#sandbox-false)).
- Remote Cards require filesystem isolation, and host-folder grants are not available on them ([Remote execution](RemoteExecution.md#host-authorization-and-following-a-run)).

## Removed: Card identity and per-call `HOME`

**Decided.** These are removed:

- the Card identity variables `TSUGUMI_RUN_ID`, `TSUGUMI_CARD` and `TSUGUMI_INVOCATION`;
- per-call changes of `HOME`.

**Today.** The worker sets all three variables and `HOME` for each call (`workerd/src/worker_execution.cc`).

## Card secrets

**Decided.** Card secrets ([#226](https://github.com/gen740/Tsugumi/issues/226)) are injected into the environment of the process once, when it starts. A change of a value recreates the worker.

**Today.**

- Each invocation receives the values in its call and sets them in the Card's process ([Card secrets](security.md#card-secrets), [remote](RemoteExecution.md#card-secrets)).
- A Card with secrets cannot keep a resident worker.

## Open

This question is not decided yet:

- Whether the UI shows guarantees that a worker cannot give.
