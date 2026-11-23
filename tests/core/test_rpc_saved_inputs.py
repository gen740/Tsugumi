"""A child Workflow can reuse saved arrivals across partial runs."""

from harness.scenario import Project
from projects import card, card_in_card, conn, run_and_wait, write
from test_rpc_scheduler import NUMBER, add_code, record
from tsugumi_client import text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState


def test_subgraph_replays_saved_merge_arrival(core_factory, project, tmp_path):
    fixture = Project(project)
    payload = b"opaque saved arrival"
    add_code(fixture, tmp_path, "source", outputs=NUMBER, values=[("out", payload)])
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
    daemon = core_factory(mocked_builds=True)
    client = daemon.connect()
    _, status = run_and_wait(client, "child", propagate=False)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.input_missing
    run, status = run_and_wait(client, "start")
    assert status.state == RunState.succeeded, status.failure
    assert bytes(record(client, run, "child/relay").outputs[0].inlineBytes) == payload

    daemon.restart()
    client = daemon.connect()
    for _ in range(2):
        run, status = run_and_wait(client, "child", propagate=False)
        assert status.state == RunState.succeeded, status.failure
        value, = record(client, run, "child/relay").outputs
        assert text(value.rootType) == "Number"
        assert bytes(value.inlineBytes) == payload
