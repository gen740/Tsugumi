# Shared fixtures

Update fixtures only when the expected public behaviour changes. The suite commands are in [Repository tests](../README.md).

- `model_fixture.py` supplies a deterministic model endpoint for Agent RPC tests; keep its responses aligned with the scenario being tested.
- `merge-contract.json` covers named records, ordered arrays and connections as sets.
- `delegateSchema` in `agent-contract.json` must match `agent/Shared/agent.json`; `tests/static/test_agent_contract.py` checks it.
- `arrange-demos.json` contains fixed graph samples; refresh deliberately with `python3 tests/python/arrange_cases.py --write`.
- `layout-golden.json` checks core layout results. Regenerate only for an intended layout change and review the resulting coordinates.
- `wire-routes.json`: after changing routing, regenerate with `WIRE_ROUTES_WRITE=1 npm --prefix web test`.
- `port-grid-contract.json` and `nix-source-refs.json` are shared by native and Web UI tests; update both consumers when changing their contracts.
