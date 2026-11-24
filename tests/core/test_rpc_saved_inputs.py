"""A child Workflow can reuse saved arrivals across partial runs."""

from harness.fixtures import mock_plan
from harness.scenario import Project
from test_run_lists import scripted_implementations
from projects import card, card_in_card, conn, run_and_wait, write
from test_rpc_scheduler import NUMBER, record
from tsugumi_client import new_id, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState


def test_subgraph_replays_saved_merge_arrival(core_factory, project, tmp_path):
    fixture = Project(project)
    packages = scripted_implementations(tmp_path / "packages", [
        [{"op": "output", "port": "out", "type": "uint64", "value": 7}],
    ])
    fixture.card("source", {"type": "code", "sandbox": {"filesystem": False}},
                 outputs=NUMBER, plan=mock_plan(packages))
    source, files = fixture.cards["source"]
    write(
        project,
        defs={
            "start": {"type": "trigger"},
            "source": source,
            "relay": {"type": "merge", "inputs": [{"name": "in"}], "recordIO": "on"},
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
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    _, status = run_and_wait(client, "child", propagate=False)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.input_missing
    source_runs = []
    payloads = []
    for _ in range(2):
        run, status = run_and_wait(client, "start")
        assert status.state == RunState.succeeded, text(status.failure.diagnostic)
        value, = record(client, run, "child/relay").outputs
        payloads.append(bytes(value.inlineBytes))
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
        assert bytes(value.inlineBytes) == payloads[-1]
