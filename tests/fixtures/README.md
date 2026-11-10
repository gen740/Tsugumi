# The shared behaviour contract of the UI and the Agent

macOS (Swift) and the Web (TypeScript) are checked against the same JSON inputs and expected values. Do not change an expected value to fit only one implementation; when the meaning changes, update both implementations and the fixture together.

- `modelOptions` in `agent-contract.json`: for each combination of connection, model, and the effort and Fast mode chosen in the menu, the choices (levels, default, Fast's price multiplier) and which fields go into the request body and which do not.
- `agent-tools.json`: the list of RPCs given to the Agent as tools (name, description, input schema, whether it changes something). Checks that Swift (`ContractTests.swift`) and the Web (`web/server/contract.ts`) make the same list. After changing the RPC, regenerate it with `npm run contract -- --update`.
- `model_fixture.py`: a local stand-in that replaces only the model connection (in the shape of the Anthropic Messages API, SSE). It decides its responses from the conversation it receives, so the Agent's disconnection, restart and resumption can be checked in both implementations.
- `merge-contract.json`: simultaneous edits of named records; the order, duplicates, deletions and additions of arrays; connections treated as a set.
- `recovery` in `agent-behavior.json`: OperationGet's reply / value and the expected error state. `error: null` is undetermined (keeps sent), `false` succeeded, `true` failed. Both implementations encode / decode responses from the real BFBS and check the Agent's recovery and the history after saving.
- `compaction` in `agent-behavior.json`: messages in the stored format and the compaction boundary cut. Checks that tool calls and results are never split, and that what is compacted together with the messages kept gives back the original history.
- `helperBrief` in `agent-behavior.json`: from a ProjectSnapshot's values and readable files, the text a helper first receives: its own Cards, wires, the upstream Cards that make their values (passing through forks), and where they are read.
- `arrange-demos.json`: samples of Card graphs taken from every scope of the bundled demos at one point in time (port names as a real tsugumid resolved them). They need not be remade when demos change. To refresh the samples, make them with `python3 tests/python/arrange_cases.py --write`.
- `arrange-cases.json`: shapes the demos do not have, written by hand: isolated Cards, folding, feedback, the boundaries of Cards in Cards, the order of wires and so on.
- `arrange-contract.json`: the two above passed through `CardGeometry` in both flow directions as inputs, with the expected results of arranging. Both implementations check that the input (geometry) and the result agree. After intentionally changing `CardGeometry` or arranging, regenerate with `cd web && npm test -- --update-arrange`.
- `wire-routes.json`: wiring inputs (flow direction, obstacles, the wire's ends) and the routes both implementations find alike. The inputs are samples taken from the demos at one point in time and need not be remade when demos change. After intentionally changing the route search, regenerate with `WIRE_ROUTES_WRITE=1 npm test`.
- `port-grid-contract.json`: expected values for snapping to the Canvas's grid and for where a Card's ports sit on the grid (`UILogicTests.swift` and `web/tests/portGrid.ts`).
- `nix-source-refs.json`: expected values for the name, the reference before pinning and the short commit made from a `nixSources` reference (`UILogicTests.swift` and `web/tests/nixSources.ts`).

Running:

```sh
npm --prefix web test
# Swift needs Xcode and flatc on macOS
(cd macOS/CoreTests && swift test --filter 'RecoveryTests|CompactionTests')
(cd macOS && swift test --filter mergePreservesNamedRecordsAndOrderedValues)
(cd macOS && swift test --filter ArrangeTests)
```

The Linux / macOS jobs of `.github/workflows/ci.yml` run both sides. Persistence around the Agent's approvals, failed saves, continuing after compaction with the existing model fixture and the like are covered further by each language's integration tests.
