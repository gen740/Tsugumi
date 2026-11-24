"""A child Workflow can reuse saved arrivals across partial runs."""

from harness.fixtures import mock_plan
from harness.scenario import Project
from projects import card, card_in_card, conn, run_and_wait, write
from test_rpc_scheduler import NUMBER, record
from tsugumi_client import new_id, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState


def test_subgraph_replays_saved_merge_arrival(core_factory, project, tmp_path):
    fixture = Project(project)
    payload = b"opaque saved arrival"
    store = tmp_path / "store"
    package = store / "source"
    input_root = package / "share/tsugumi/input"
    ports = input_root / "ports"
    ports.mkdir(parents=True)
    (ports / "out").write_bytes(payload)
    fixture.card("source", {"type": "input"}, outputs=NUMBER,
                 plan=mock_plan([str(package)]))
    source, files = fixture.cards["source"]
    write(
        project,
        defs={
            "start": {"type": "trigger"},
            "source": source,
            "relay": {"type": "merge", "inputs": [{"name": "in"}], "recordIO": True},
        },
        cards=[
            card("start", "start"),
            card("source", "source"),
            card_in_card("child", [card("relay", "relay")], [],
                         {"inputs": {"in": {"card": "relay", "port": "in"}},
                          "outputs": {"out": {"card": "relay", "port": "out"}}}),
        ],
        conns=[conn("source", "out", "child", "in")],
        files={f"definitions/source/{name}": content for name, content in files.items()},
    )
    daemon = core_factory(mocked_builds=True, env={"TSUGUMI_MOCK_STORE": str(store)})
    client = daemon.connect()
    _, status = run_and_wait(client, "child", propagate=False)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.input_missing
    source_runs = []
    for _ in range(2):
        run, status = run_and_wait(client, "start")
        assert status.state == RunState.succeeded, text(status.failure.diagnostic)
        value, = record(client, run, "child/relay").outputs
        assert bytes(value.inlineBytes) == payload
        source_runs.append(run)
    deleted = client.call("RunHistoryDeleteRequest", restrictToRunIds=True,
                          runIds=source_runs[:1], opId=new_id())
    assert deleted.deletedRuns == 1

    daemon.restart()
    client = daemon.connect()
    for _ in range(2):
        run, status = run_and_wait(client, "child", propagate=False)
        assert status.state == RunState.succeeded, text(status.failure.diagnostic)
        value, = record(client, run, "child/relay").outputs
        assert text(value.rootType) == "Number"
        assert bytes(value.inlineBytes) == payload
