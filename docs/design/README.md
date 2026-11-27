# Tsugumi design

Start with [Architecture](Architecture.md) for the program boundaries, or
[Rationale](Rationale.md) for the decisions that shape the Workflow.
The [user guides](../README.md) cover using the workbench and writing Cards.

## Architecture and boundaries

| Document | Contents |
| --- | --- |
| [Architecture](Architecture.md) | Program ownership, Host environment services and the contracts between layers |
| [Rationale](Rationale.md) | Why people and the Agent design, run and inspect the same Workflow |
| [Security and isolation](security.md) | VM, project and connection boundaries, grants and data access |
| [RPC contract](Protocol.md) | Generated Project, Host and Agent request and event reference |

## Card execution

| Document | Contents |
| --- | --- |
| [Cards and Workflows](Workflow.md) | Card kinds, port types, scheduling and batches |
| [Card environments and Nix evaluation](Nix.md) | Locked sources, restricted evaluation and reuse |
| [Core and SDKs](CoreAndSDK.md) | Builder interface, templates and view hosts |
| [Card ABI](CardABI.md) | Worker entry point, value envelopes, artifacts and display effects |
| [Sandbox contract](SandboxContract.md) | Process directories, isolation, grants and access to sealed artifacts |
| [Worker selection and remote execution](RemoteWorker.md) | Probe scoring, job admission, transfer and remote journal lifecycle |
| [Nix binary cache server](BinaryCacheServer.md) | Standalone cache protocol, atomic storage, signing and authentication |

## History and host services

| Document | Contents |
| --- | --- |
| [Run storage and recovery](RunStorage.md) | Artifact layout, interrupted runs and cleanup |
| [Version storage and recovery](VersionRecords.md) | Git records, past trial lookup and the checkout journal |
| [Shared VM lifecycle and store integrity](VMStoreHealth.md) | Guest image, shutdown and Nix store checks |
| [Agent service](Agent.md) | Tool policy, helpers, chat presentation and the guide |
| [Agent reference identifiers](ShortIdentifiers.md) | Project, run, invocation, artifact, conversation and view identifiers |
| [Host presentation services](HostPresentation.md) | Host formatting and localized version text |

## Repository guides

- [Repository map](../../AGENTS.md#map) for source ownership and development rules.
- [Worker implementation](../../workerd/README.md) for local leases and process ownership.
- [Standalone app export](../../pack/README.md) for packed execution and debug exports.
- [WebGPU imports](../../ui/shared/dawn/README.md) for generated Dawn bindings.
- [Tests](../../tests/README.md) and [execution benchmarks](../../tests/bench/README.md) for verification.
- [Documentation site](../site/README.md) and [demo player](../site/player/README.md) for publication and recording.
- [Release procedures](../../RELEASING.md) for building and distributing Tsugumi.
