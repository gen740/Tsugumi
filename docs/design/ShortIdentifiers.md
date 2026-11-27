# Agent reference identifiers

The Agent addresses the same project records as the clients. Short, stable
references let a reader follow a cited result without carrying a separate
identifier map. Authorization follows the project's grants and the caller's
scope on every read or operation.

| Record | Identity and scope |
| --- | --- |
| Project | Six lower-case characters from `23456789abcdefghjkmnpqrstuvwxyz`. Host-managed projects reserve the ID before startup. A direct core can allocate its own ID in `.tsugumi/project-id`. |
| Card | Its definition ID, or a slash-joined path such as `outer/step` for a nested Card. [Project files](../Reference.md#card-ids-and-port-names) defines the syntax. |
| Run and invocation | A positive decimal run number within the project, and a Card path and invocation index within the run. An evidence link can address `run:12/outer/step#2`. The run counter survives deletion of run records because it is stored separately in project history. |
| Log and saved value | Run, Card and invocation, with a stream and byte offset for a log, or an input/output port name for a value. [Evidence links](../RunHistory.md#follow-evidence-from-chat) open the associated record. |
| Artifact | A 16-byte UUID carried in the value envelope. The project resolves it to a sealed tree. [Artifact access](SandboxContract.md#artifact-access) defines the scope. |
| Recorded environment | A number within the run. Invocations with the same recorded derivation, manifest and system share a record. |
| Conversation | A six-character random ID allocated by the host. The guide and external tool session use named conversations. |
| Tool and helper calls | The host reserves decimal call IDs within a conversation. Helper call IDs include the parent call and task index, as in `1.0.2`. The counters persist across compaction and restart. |
| Git version | A Git object ID. The host presents an abbreviation that Git resolves if unambiguous. |
| View session | A UUID identifying the open window. The core uses separate route UUIDs to address Card mailboxes. [Display messages](CardABI.md#display-messages) defines the lifetimes. |

The host uses the conversation and call identity when constructing operation
receipt keys. The core accepts bounded, filename-safe opaque keys. Transport
credentials, authorization grants and remote job identities have their own
scope and lifetime.
