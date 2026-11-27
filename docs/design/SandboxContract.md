# The sandbox contract

The worker constructs a process environment for the Card. Linux uses bubblewrap
for namespace isolation. Native macOS uses Seatbelt. The Card's shared library
runs in the worker's address space.

## The worker builds what the Card sees

The working directory, `HOME` and `TMPDIR` are fixed at process launch. The
worker sets `PATH` to the Card package's `bin` directory when present. Declare
external programs through the SDK builder's `tools` attribute. To run a
shell, declare it too. The TLS trust bundle comes from Nix's `cacert`.
The project core attaches run and Card identity to display messages.
See [Card ABI](CardABI.md#effects-and-their-lifetimes).

## The unit of isolation is the Card

Each worker has its own home and working directory. A resident worker
(`reuseWorker`) takes one call at a time and can retain its state between
invocations and runs of the same Card. It is replaced when the definition
or execution settings change, or after a failure, cancellation or timeout.
An active invocation records the definition it used. See [Worker state](../CardGuide.md#worker-state).

## Directories

| Directory | Lifetime and contents |
| --- | --- |
| Working directory and `HOME` | Private writable storage for the lifetime of the process. |
| `TMPDIR` | Private writable path. Its contents are cleared between invocations. With Linux filesystem isolation, bubblewrap binds disk-backed scratch at `/tmp`. On native macOS, use `TMPDIR`. |
| Artifact inputs and outputs | The supervisor creates call-scoped paths. See [Artifact access](#artifact-access). |

## Artifact access

A Card can open any sealed artifact of its project by UUID. The UUID is the
capability. The returned read-only path is scoped to the invocation.

The artifact store stays outside the sandbox. The worker supervisor creates
a private invocation directory on the same filesystem and materializes
the requested tree when the Card calls `artifact_open`. It recreates directory
trees and hard-links their files on Linux, including in the shared VM, with a copy
fallback across filesystems.
Bubblewrap mounts those inputs read-only. On native macOS the supervisor
clones files, and Seatbelt denies writes and metadata changes to the inputs.
The supervisor installs created outputs under
`.tsugumi/artifacts/<run>/<card path>/@<invocation index>/<counter>/` and
seals each tree. A fresh Linux worker is retired before its output is
renamed into the store; with PID isolation that stops every process the Card
started, and without it nothing is guaranteed about a process that outlives
the call. Native macOS has no PID namespace, so it seals a `clonefile(2)`
clone of the output: new inodes that share blocks copy-on-write, out of reach
of any writer that outlived the call. The project must be on APFS there; on
another filesystem the call fails instead of copying. A resident
(`reuseWorker`) keeps running, so its outputs are also sealed as new inodes:
a clone on macOS, and on Linux a reflink (`FICLONE`) where the filesystem has
one and a copy otherwise. In the shared VM the store is a Mac folder seen
through virtiofs, which has no reflinks, so a resident's outputs are copied.

When a call returns, the supervisor removes its input and output directories
from the worker's private artifact directory. That directory is bound once,
at a fixed path, into the worker's sandbox, so the removal takes the paths out
of a resident's view without remounting, and the next call's directories
appear in the same place. Descriptors opened during a call cannot be revoked:
a leftover input descriptor continues to read the artifact it opened, and a leftover
output descriptor points at an inode the sealed artifact has superseded.
Remote workers request sealed artifacts on demand and return newly created
artifacts with the result. Artifacts made without KEEP expire at run end. These access guarantees require filesystem
isolation.

## Grants

A Card declares network, GUI or host path grants in `_card.json`. Each named
host path refers to a declaration in that Card's project. The person binds
the name to a folder in host settings. Linux workers with filesystem
isolation mount it at `/host/<name>` and expose `TSUGUMI_GRANT_<NAME>` to the
Card. A `write: false` grant makes the mount read-only. On a Mac, use the
shared Linux VM for this grant. The GUI grant requires a native Linux worker
with a usable Wayland or X11 socket.

Granted I/O is a side effect of the Card. For example, a network grant lets
the Card retrieve external data. Tsugumi's own run traffic uses the project
core and worker channels. See [permission approval](security.md#the-agents-permission-boundary)
and [the run-data policy](../Philosophy.md#a-laboratory-notebook-and-who-decides-what-is-kept).

## Isolation options

The Card applies its enabled settings together. A worker must satisfy the
requested settings to be eligible for selection.

| Setting | Effect |
| --- | --- |
| `sandbox.filesystem` | Restricts reads and writes to the worker's runtime, scratch, inputs and grants. On native macOS the store and selected system paths stay readable. |
| `sandbox.pid` | Confines process inspection and signals to the worker's process tree. |
| `sandbox.ipc` | Restricts host shared-memory, semaphore, message-queue and socket access. |
| `sandbox.identity` | Separates the worker from the user's credentials. Linux maps uid and gid 65534 and drops capabilities. Native macOS applies Seatbelt restrictions with the launch uid. |
| `sandbox.limits` | Applies nonzero CPU and memory limits through delegated cgroup v2 controllers. |
| `grants.network` | Permits network access within the worker's other enabled restrictions. |

The shared VM hides its control files independently of the Card's settings.
Worker teardown ends the worker's process group. See [platform boundaries](security.md#4-card-worker--tsugumid)
and [remote worker eligibility](RemoteWorker.md#worker-selection).

## Card secrets

The worker receives the declared secret values at process launch and exposes
them as environment variables. Changing a value replaces a resident worker.
See [secret handling](security.md#card-secrets) and [remote delivery](RemoteWorker.md#card-secrets).
