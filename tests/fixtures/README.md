# Shared fixtures

Update fixtures only when the expected public behaviour changes. The suite commands are in [Repository tests](../README.md).

- `model_fixture.py` supplies a deterministic model endpoint for Agent RPC tests; keep its responses aligned with the scenario being tested.
- `delegateSchema` in `agent-contract.json` records the structure of the schema in `hostd/agent/Shared/agent.json` without description text; `tests/static/test_agent_contract.py` checks it.
- `layout-golden.json` checks core layout results. Regenerate only for an intended layout change and review the resulting coordinates.
- `wire-routes.json`: after changing routing, regenerate with `WIRE_ROUTES_WRITE=1 npm --prefix ui/web test`.
- `port-grid-contract.json` and `nix-source-refs.json` are shared by native and Web UI tests; update both consumers when changing their contracts.

- `subgraph-edits.json` records host document edits for Web gesture and undo fixtures. The host unit tests verify every reply; graph transformation expectations belong to `tests/core/agent/document_edits_test.cc`. Presentation fixtures call the production operation executor through `tsugumi_document_edit_driver` (override its path with `TSUGUMI_DOCUMENT_EDIT_DRIVER`).

- `service-contract.json` contains flatc wire goldens for Host reference demos/version notes, Agent summaries/access and shared replies. C++, Swift and TypeScript read the same vectors; the Agent request list also checks the BFBS-derived external MCP catalog. Regenerate with `python3 tests/tools/generate_service_vectors.py` and review the wire changes.
