# The sandbox contract

**Status.** The owner decided this contract on 2026-11-13 and 2026-11-21. The P1 sandbox work and the Card ABI cutover implement it.
Each **Before P1** note records the behavior at `24fc2d2c`, the revision the contract was written against, as evidence of what changed. [Security §4](security.md#4-card-worker--tsugumid) describes the current implementation. [Open](#open) lists the remaining decision.

## The worker builds what the Card sees

**Decided.**

- The worker builds what the Card sees.
- The contract fixes meanings, not path strings.
- Each kind of worker implements the contract with its own means: bubblewrap on Linux, and Seatbelt with the environment on native macOS.
- The environment, `HOME`, `TMPDIR` and the working directory are fixed for the lifetime of the process.
- The contract states what the worker **provides**. It does not defend against the Card's own code: `main.so` shares the worker's address space.
- Nothing about identity is left for the Card to see. `tsugumid`, not the worker, stamps display messages with the Card and the run, and `WorkerCall` carries no `run_id`, invocation or Card ([Card ABI](CardABI.md#effects-and-their-lifetimes)).
- The "container" worker kind, which does not exist, is dropped. The tests use the shared VM.
- The Card's process gets no implicit shell and no implicit `PATH`: there is no `/bin/sh` for it to rely on. A Card that runs other programs declares them to its language SDK's builder (for example `tools = [ pkgs.coreutils ];`), the builder installs them in the Card package's `bin` folder, and the worker sets `PATH` to that folder alone (and leaves it unset when there is none). [The language SDK guides](https://github.com/gen740/tsugumi-kit) describe the attribute.

**Before P1 (`24fc2d2c`).**

- `WorkerCall` carries `run_id`, the Card and the invocation (`protocol/worker.fbs`).
- The working directory and the environment are set again on each call.
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

**Before P1 (`24fc2d2c`).** A resident worker lives until the project's environment stops or the Card's definition or execution conditions change. A failed, cancelled or timed-out call discards it.

## Directories

**Decided.**

| What | Writable | Lifetime | Isolated from |
| --- | --- | --- | --- |
| Working directory = `HOME` | yes | The process. With `reuseWorker` it carries across invocations and runs of the same Card | Other processes, and runs of other Cards |
| TMP | yes | The path is fixed for the process; the contents are cleared for each invocation | Other runs |

Where the worker can remap paths (bubblewrap on Linux, the shared VM and remote workers), `/tmp` is the same place as `TMPDIR`: a disk-backed directory that Tsugumi manages (scratch bound at `/tmp`), not bubblewrap's in-memory tmpfs. Programs that hard-code `/tmp` need no special care there. On native macOS only `TMPDIR` is guaranteed.

New artifacts are created in writable directories supplied by the worker. [Card ABI](CardABI.md#artifacts)

**Before P1 (`24fc2d2c`).**

- `HOME`, `XDG_CONFIG_HOME` and `XDG_CACHE_HOME` are writable worker directories.
- The invocation's input, output, work and temporary slots are emptied after each call (`WorkerProc::clear_scratch`; [pre-P1 isolation](https://github.com/gen740/Tsugumi/blob/24fc2d2c93d607e1aa608cceec444f6b7e92affb/docs/security.md#sandbox-true-the-default)).
- bubblewrap mounts an in-memory tmpfs at `/tmp`, separate from `TMPDIR` (`workerd/src/worker_proc.cc`).

## Artifact access

Each invocation receives grants for the artifact IDs listed on its input
envelopes. The SDK opens a granted artifact as a read-only directory.
The returned path is scoped to the invocation. Forwarding an ID preserves
the artifact's identity.

The artifact store stays outside the sandbox. The worker supervisor creates
a private invocation directory on the same filesystem and materializes
granted inputs when the Card calls `artifact_open`. It recreates directory
trees and hard-links their files on Linux, including in the shared VM.
Bubblewrap mounts those inputs read-only. On native macOS the supervisor
clones files, and Seatbelt denies writes and metadata changes to the inputs.
The supervisor installs created outputs under
`.tsugumi/artifacts/<run>/<card path>/@<invocation index>/<counter>/` and
seals each tree. Linux workers with PID isolation retire the writing process
namespace before renaming the output. Native macOS and Linux workers without
PID isolation snapshot the output before installing it.

A call with a project artifact grant uses a fresh worker. A call that creates
an artifact retires its worker after completion. Input Card artifacts retain
their immutable Nix store paths. Remote workers stage grants and return newly
created artifacts with the result. These access guarantees require filesystem
isolation.

## Grants

**Decided.**

- Grants are declared in the Card's definition and given by a person. They are:
  - host paths, read-only or read-write;
  - the GUI;
  - the network.
- Grants reach the Card as environment variables.
- The TLS CA is confined to the Card. It defaults to Nix's `cacert` and never comes from the launcher's environment.
- **I/O that the Card starts through a grant is an explicitly permitted side effect.** For example, a Card can download from the network or write into a read-write host path. "Run data travel only on `tsugumid`'s channel" ([The core and the language SDKs](CoreAndSDK.md#philosophy-constraints-that-still-hold)) covers Tsugumi's own transport, not what a Card does through its grants.

**Before P1 (`24fc2d2c`).**

- `grants.network` and optional `grants.hostPaths` are set in `_card.json`. A host folder with `write: false` is mounted read-only at `/host/<name>`. See [Outside access](features.md#outside-access).
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

**Before P1 (`24fc2d2c`).**

- `sandbox` (on by default) is a single Card setting, and `sandbox: false` is a separate mode ([`sandbox: false` before P1](https://github.com/gen740/Tsugumi/blob/24fc2d2c93d607e1aa608cceec444f6b7e92affb/docs/security.md#sandbox-false)).
- An Agent's save that turns the sandbox off, allows the network or adds a host folder waits for a person's approval ([The Agent's permission boundary](security.md#the-agents-permission-boundary)).
- Seatbelt on native macOS honours `filesystem` and `network` only partly, and has none of `pid`, `ipc` or `limits`.
- Local Linux workers have no CPU or memory limits, only a deadline.
- Remote Cards require filesystem isolation, and host-folder grants are not available on them ([Remote execution](RemoteExecution.md#host-authorization-and-following-a-run)).

## What each kind of worker did before P1

These are facts about the code at `24fc2d2c`, kept for comparison with the contract.

| Kind | What it does |
| --- | --- |
| Linux native (bubblewrap) | Host paths bound at `/host/<name>`; network on or off; mount, PID, IPC and user namespaces, uid 65534; no CPU or memory limits, a deadline only; no GUI |
| macOS native (Seatbelt) | `grants.hostPaths` is rejected. With `grants.network: false` the network is denied. With `grants.network: true` the LAN is reachable and localhost and Unix sockets stay denied. No path remapping and no PID or IPC namespaces. Reads of `/`, `/usr`, `/System` and `/private/etc` are allowed, and metadata of the whole host is visible. POSIX shared memory is allowed. No limits. Without the sandbox, a plain `exec` |
| Shared VM (the Linux guest on a Mac) | Mac folders through virtiofs and bubblewrap; limits only for the whole VM; no display path to the Mac |
| Remote (`tsugumi-workerd`) | Nested bubblewrap, uid 65534, cgroup v2 `cpu.max` / `memory.max`; no host paths; running without the sandbox and `reuseWorker` are rejected |

## Removed environment variables

**Decided.** These are removed:

- the Card identity variables `TSUGUMI_RUN_ID`, `TSUGUMI_CARD` and `TSUGUMI_INVOCATION`;
- `TSUGUMI_PARAMS_JSON`;
- the `TSUGUMI_*_DIR` variables;
- the per-call change of `HOME`;
- the implicit `PATH` and `/bin/sh` that the worker supplied to every Card.

The Python SDK sets `MPLCONFIGDIR` itself.

**Before P1 (`24fc2d2c`).**

- The worker sets the identity variables, `TSUGUMI_PARAMS_JSON` and `HOME` for each call (`workerd/src/worker_execution.cc`).
- `MPLCONFIGDIR` is set by `workerd/src/worker_proc.cc`.

## Card secrets

**Decided.** Card secrets ([#226](https://github.com/gen740/Tsugumi/issues/226)) are injected into the environment of the process once, when it starts. A change of a value recreates the worker.

**Before P1 (`24fc2d2c`).**

- Each invocation receives the values in its call and sets them in the Card's process ([Card secrets](security.md#card-secrets), [remote](RemoteExecution.md#card-secrets)).
- A Card with secrets cannot keep a resident worker.

## Open

This question is not decided yet:

- Whether the UI shows guarantees that a worker cannot give.
