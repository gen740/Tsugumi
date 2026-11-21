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
- The contract states what the worker **provides**. It does not defend against the Card's own code: `main.so` shares the worker's address space.
- So that nothing about identity is left for the Card to see, the identity wrapping of displays moves from the worker to `tsugumid`, and `WorkerCall` drops `run_id`, `invocation` and the Card's identity ([Card ABI 9](CardABI.md#effects-and-their-lifetimes)).
- The "container" worker kind, which does not exist, is dropped. The tests use the shared VM.

**Today.**

- `WorkerCall` carries `run_id`, the Card and the invocation (`protocol/worker.fbs`).
- The working directory and the environment are set again on each call ([Worker state](CardGuide.md#worker-state)).
- Each call also sets `HOME` to the worker's work directory.

## The unit of isolation is the Card

**Decided.**

- A run of one Card never sees another Card's `HOME` or state.
- With `reuseWorker`, a Card may carry state across its own runs. This is acceptable for the same reason that memory carries over.
- A reused worker:
  - ends when the series of runs ends;
  - is recreated when the Card's definition, language SDK or grants change;
  - takes one call at a time.
- When a definition changes:
  - new calls go to a new worker;
  - a call that is already running finishes on the old version and records the version it used.
- A failed attempt is recorded even though its outputs are discarded.

**Today.** A resident worker lives until the project's environment stops or the Card's definition or execution conditions change. A failed, cancelled or timed-out call discards it ([Worker state](CardGuide.md#worker-state)).

## Directories

**Decided.**

| What | Writable | Lifetime | Isolated from |
| --- | --- | --- | --- |
| Working directory = `HOME` | yes | The process. With `reuseWorker` it carries across invocations and runs of the same Card | Other processes, and runs of other Cards |
| TMP | yes | The path is fixed for the process; the contents are cleared for each invocation | Other runs |

Where the worker can remap paths (bubblewrap on Linux, the shared VM and remote workers), `/tmp` is the same place as `TMPDIR`: a disk-backed directory that Tsugumi manages (scratch bound at `/tmp`), not bubblewrap's in-memory tmpfs. Programs that hard-code `/tmp` need no special care there. On native macOS only `TMPDIR` is guaranteed.

File and Directory outputs are created under the working directory ([Card ABI 9](CardABI.md#file-and-directory-outputs)).

**Today.**

- `HOME`, `XDG_CONFIG_HOME` and `XDG_CACHE_HOME` are writable worker directories.
- The invocation's input, output, work and temporary slots are emptied after each call (`WorkerProc::clear_scratch`; [Security §4](security.md#sandbox-true-the-default)).
- bubblewrap mounts an in-memory tmpfs at `/tmp`, separate from `TMPDIR` (`workerd/src/worker_proc.cc`).

## File and Directory inputs

**Decided.**

- The input value holds an absolute path.
  - With the `filesystem` option on, the Card's view of the input is read-only.
  - With `filesystem` off, the input is a copy (below), so the Card can write to that copy. Only the stored original is protected.
- "Valid for that invocation" is the lifetime of the path. File descriptors that the Card keeps open are not revoked.
- Inputs are hardlinked; a Directory is hardlinked file by file.
- Inputs are copied instead of hardlinked:
  - across filesystems;
  - whenever read-only cannot be enforced. A hardlink shares the inode of the stored run data, so without read-only enforcement the Card could change those data;
  - always when the `filesystem` option is off, because a hardlink would let the Card own the inode and write to it;
  - always on native macOS.
- With `reuseWorker`, the inputs are in a read-only bound directory, and the worker swaps its contents for each invocation.

**Today.** The core copies the invocation's File and Directory inputs into a fixed, read-only staging area local to the worker ([Security §4](security.md#sandbox-true-the-default)).

## Grants

**Decided.**

- Grants are declared in the Card's definition and given by a person. They are:
  - host paths, read-only or read-write;
  - the GUI;
  - the network.
- Grants reach the Card as environment variables.
- The TLS CA is confined to the Card. It defaults to Nix's `cacert` and never comes from the launcher's environment.
- **I/O that the Card starts through a grant is an explicitly permitted side effect.** For example, a Card can download from the network or write into a read-write host path. "Run data travel only on `tsugumid`'s channel" ([The core and the language SDKs](CoreAndSDK.md#philosophy-constraints-that-still-hold)) covers Tsugumi's own transport, not what a Card does through its grants.

**Today.**

- `network` and `hostFolders` are set in `_card.json`. A host folder is mounted at `/host/<name>`, read-only by default ([Outside access](features.md#outside-access-experimental)).
- There is no GUI grant.
- For a Card with the network, the worker passes on `SSL_CERT_FILE` and `NIX_SSL_CERT_FILE` from its own environment, which it inherits from its launcher (`workerd/src/worker_proc.cc`, `workerd/src/local_client.cc`).

## Sandbox options and grants are on/off settings

**Decided.**

- Sandbox options and grants are plain on/off settings, shown as individual checkboxes.
- The options, for now (more can be added later):

  | Option | Meaning |
  | --- | --- |
  | `filesystem` | Only the Nix closure, scratch, inputs and enabled host paths are visible |
  | `network` | Network access |
  | `pid` | A separate PID namespace |
  | `ipc` | A separate IPC namespace |
  | `identity` | uid 65534, with capabilities dropped |
  | `limits` | cgroup v2 CPU and memory limits, on local Linux workers too |
  | host paths | Host paths, read-only or read-write |
  | GUI | The GUI |
- There is no permission dialog.
- A grant that is off (network, a host path, the GUI) makes the operation fail inside the Card.
- An isolation option that is off (`filesystem`, `pid`, `ipc`, `identity`, `limits`) removes that restriction.
- There is no "declared but not granted" state.
- Turning every isolation option off means no sandbox; there is no separate no-sandbox mode.
- The options belong to the worker, and they are part of the contract.
- When a Card's setting conflicts with what the chosen worker supports, the UI asks the person to remove that option.
- These worker invariants hold even with every option off:
  - in the VM, hostd's tokens are hidden from the Card;
  - the Card's processes are killed as a process group.

**Today.**

- `sandbox` (on by default) is a single Card setting, and `sandbox: false` is a separate mode ([`sandbox: false`](security.md#sandbox-false)).
- An Agent's save that turns the sandbox off, allows the network or adds a host folder waits for a person's approval ([The Agent's permission boundary](security.md#the-agents-permission-boundary)).
- Seatbelt on native macOS honours `filesystem` and `network` only partly, and has none of `pid`, `ipc` or `limits`.
- Local Linux workers have no CPU or memory limits, only a deadline.
- Remote Cards require filesystem isolation, and host-folder grants are not available on them ([Remote execution](RemoteExecution.md#host-authorization-and-following-a-run)).

## What each kind of worker does today

These are facts about the code at origin/main 24fc2d2c, for comparison with the contract.

| Kind | What it does |
| --- | --- |
| Linux native (bubblewrap) | Host paths bound at `/host/<name>`; network on or off; mount, PID, IPC and user namespaces, uid 65534; no CPU or memory limits, a deadline only; no GUI |
| macOS native (Seatbelt) | `hostFolders` rejected. Network denied by default; when allowed, localhost and Unix sockets stay denied but the LAN is reachable. No path remapping and no PID or IPC namespaces. Reads of `/`, `/usr`, `/System` and `/private/etc` are allowed, and metadata of the whole host is visible. POSIX shared memory is allowed. No limits. Without the sandbox, a plain `exec` |
| Shared VM (the Linux guest on a Mac) | Mac folders through virtiofs and bubblewrap; limits only for the whole VM; no display path to the Mac |
| Remote (`tsugumi-workerd`) | Nested bubblewrap, uid 65534, cgroup v2 `cpu.max` / `memory.max`; no host paths; running without the sandbox and `reuseWorker` are rejected |

## Removed environment variables

**Decided.** These are removed:

- the Card identity variables `TSUGUMI_RUN_ID`, `TSUGUMI_CARD` and `TSUGUMI_INVOCATION`;
- `TSUGUMI_PARAMS_JSON`;
- the `TSUGUMI_*_DIR` variables;
- the per-call change of `HOME`.

`MPLCONFIGDIR` moves to the Python SDK.

**Today.**

- The worker sets the identity variables, `TSUGUMI_PARAMS_JSON` and `HOME` for each call (`workerd/src/worker_execution.cc`).
- `MPLCONFIGDIR` is set by `workerd/src/worker_proc.cc`.

## Card secrets

**Decided.** Card secrets ([#226](https://github.com/gen740/Tsugumi/issues/226)) are injected into the environment of the process once, when it starts. A change of a value recreates the worker.

**Today.**

- Each invocation receives the values in its call and sets them in the Card's process ([Card secrets](security.md#card-secrets), [remote](RemoteExecution.md#card-secrets)).
- A Card with secrets cannot keep a resident worker.

## Open

This question is not decided yet:

- Whether the UI shows guarantees that a worker cannot give.
