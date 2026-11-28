# Architecture

The source tree follows program ownership: `tsugumid/document` contains definition
contracts; `tsugumid/runtime` owns project execution/history; `hostd` owns human
and Agent policy; `workerd` owns worker lifetimes and invocation values.
`util` and `rpc` provide mechanisms without importing those program policies.
The guest is built under `vm/guest`, the macOS VM helper under
`vm/macos/runtime`, and protocol-aware Swift clients under `client/swift`.
UI implementations are `ui/web` and `ui/macos`, with isolated module resources
in `ui/shared`. The macOS VM helper owns Virtualization.framework in its own Swift package. `tsugumi-vmctl` is a Host RPC client for environment operations.

The shared RPC reflection schema belongs to `protocol`. The CMake and static boundary checks verify production include and link ownership.

See the [trust boundaries](security.md#1-the-trust-boundaries), [RPC contract](Protocol.md) and [repository map](../../AGENTS.md#map).

The host-side `tsugumi-hostd` exposes environment operations through `EnvironmentRequest` in `protocol/host.fbs`: state, start, stop and attached-folder changes. Its native adapter launches `tsugumid` on Linux; its signed Mac adapter boots the shared VM and applies the mounts hostd specifies. In both cases, the service owns readiness checks, endpoint and project registration, execution-broker authorization, concurrent starts and process-exit reporting. Host events carry state, progress and stopped notifications. `HostRuntimeRequest` reports runtime and store state without starting a VM.

The shared VM's CPUs, memory and store disk size are host settings (`vm.cpus`, `vm.memoryGiB`, `vm.storeGiB`), edited under **External Connections** in the Mac app's Settings and under **General** in the Web UI. The value 0 uses this Mac's defaults: all but two cores (2–8), half the memory (4–16 GiB) and a 64 GiB store. Changes apply at the VM's next start. A larger store grows the disk in place; a smaller one keeps the current disk until the environments are reset. After two minutes with no project running in the VM and no store operation, the VM's memory balloon takes the guest down to 1.5 GiB: the guest drops its page cache and macOS gets the memory back. Before starting a project, verifying the store or collecting garbage, the host sets the balloon's target back to the configured size and requests a guest memory snapshot on the control stream.

The Mac app, the Web UI and the `tsugumi` command use the same Host environment requests and events. `tsugumi-vmhost` exposes the private length-framed FlatBuffers contract in `protocol/vm_host.fbs` to hostd. It retains Virtualization.framework, virtiofs/vsock and terminal descriptor bridges because they need the app’s signature and entitlement. Clients display runtime snapshots and exchange terminal bytes through Host requests; they do not connect to the helper.

Inside the VM, a project's core has a PID namespace of its own and cannot see the one `nix-daemon` that builds for every project. Its Nix build client therefore asks `tsugumi-vmd` for the CPU time of the daemon processes serving it, on the Unix socket `/run/tsugumi-build-cpu` (`protocol/vm_project.fbs`, `TSUGUMI_VM_BUILD_CPU` in the core's environment). The request names no process: vmd takes the connecting process's pid and uid from the socket's peer credentials, which the kernel gives in the guest's PID namespace, the one where `nix-daemon` also names its process for that client by the client's pid. So neither side translates a pid between namespaces, and both match by the same kernel-given number. vmd answers only a running project's process (its uid is the project's and it is still in that project's cgroup), only about that process, so a project learns nothing of another project's processes. The answer is the CPU time of those daemon processes, with their builders and the builders they reaped, as the client counts it itself on a Linux host ([RemoteWorker](RemoteWorker.md)).

[Shared VM lifecycle and store integrity](VMStoreHealth.md)

## Documents and saving

[Project files](../Reference.md) defines the file formats. [ProjectSave](Protocol.md#projectsave) defines save transactions and conflicts; [Versions](../Versions.md) explains returning to saved code and settings.

## Contract boundaries

| Boundary | Owning contract |
| --- | --- |
| Project definitions | The core owns [project](../../protocol/project_document.fbs), [lock](../../protocol/project_lock.fbs) and [Card](../../protocol/card_document.fbs) documents and generates input schemas. See [project files](../Reference.md). |
| Definition evaluation | The core supplies parameters and upstream Nix exports. The SDK builds language-specific libraries, typed interfaces and views. See [the builder interface](CoreAndSDK.md#the-builder-interface). |
| Card and worker | The [Card ABI](CardABI.md) fixes the value envelope and effects. The SDK interprets application data. |
| Worker and core | The [worker protocol](../../protocol/worker.fbs) carries invocations, artifact create/open exchanges and opaque view bytes. The [remote worker protocol](../../protocol/remote_worker.fbs) covers on-demand job transfer. |
| Core and clients | The [RPC contract](Protocol.md#views) carries view declarations, blob pages, messages and credits. |
| Demo definitions and player | The [demo player](../site/player/README.md) resolves ports against the demo definitions and uses recorded values for playback. |
