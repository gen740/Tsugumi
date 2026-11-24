# Card contract references

Use the owning source for changes at each boundary.

| Boundary | Contract and ownership |
| --- | --- |
| Project definitions | Core owns the FlatBuffers [project](../../protocol/project_document.fbs), [lock](../../protocol/project_lock.fbs) and [Card](../../protocol/card_document.fbs) documents and generates input schemas. See [the definition guide](../CardGuide.md#card-definition-reference). |
| Definition evaluation | Core passes evaluated parameters and upstream Nix exports to the definition. The kit builds language-specific libraries, typed interfaces and views. See [the builder interface](../CoreAndSDK.md#the-builder-interface). |
| Card ↔ worker | [Card ABI](../CardABI.md) covers the fixed envelope, artifacts and the effects table. The SDK interprets the application payload. |
| Worker ↔ core | The [worker protocol](../../protocol/worker.fbs) carries invocations, artifact create/open exchanges and opaque view bytes. On-demand transfer uses the [remote worker protocol](../../protocol/remote_worker.fbs). |
| Core ↔ UI | The [RPC contract](../Protocol.md) carries declarations, pages of view blobs, display bytes and credits. The UI resolves view names. See [Views](../CoreAndSDK.md#views). |
| Demo sources ↔ player | The player builds against current demo definitions and ports resolved by core. Recorded runs remain evidence of their original graph. See [the demo player](../site/player/README.md). |
