"""A child Workflow can reuse saved arrivals across partial runs."""

import sqlite3

from harness.fixtures import mock_plan
from harness.scenario import Project
from test_run_lists import scripted_implementations
from projects import card, card_in_card, conn, run_and_wait, write
from test_rpc_scheduler import NUMBER, record
from tsugumi_client import new_id, path, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState


def test_saved_inputs_use_per_port_records_only_for_merge(core_factory, project):
    write(
        project,
        defs={
            "start": {"type": "trigger"},
            "collect": {"type": "collect", "recordIO": "on"},
            "merge": {"type": "merge", "recordIO": "on", "maxRuns": 2,
                      "inputs": [{"name": "a"}, {"name": "b"}]},
        },
        cards=[
            card("start", "start"),
            card("collect", "collect"),
            card_in_card("child", [card("merge", "merge")], [],
                         {"inputs": {port: {"card": "merge", "port": port}
                                     for port in ("a", "b")}}),
        ],
        conns=[conn("start", "out", "collect", "in"),
               conn("start", "out", "child", "a"),
               conn("start", "out", "child", "b")],
    )
    daemon = core_factory()
    client = daemon.connect()
    run, status = run_and_wait(client, "start")
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    with sqlite3.connect(project / ".tsugumi" / "runs.sqlite") as db:
        saved = db.execute("SELECT card FROM last_inputs WHERE run_id=? ORDER BY card",
                           (run,)).fetchall()
    assert saved == [("child/merge",), ("child/merge#a",), ("child/merge#b",), ("collect",)]

    def inputs(run_id, card_path):
        rows = client.call("RunInvocationsRequest", runId=run_id,
                           card=path(*card_path.split("/"))).records or []
        return {text(value.name): bytes(value.inlineBytes)
                for row in rows
                for value in record(client, run_id, card_path, row.invocation).inputs}

    collected = inputs(run, "collect")
    merged = inputs(run, "child/merge")
    assert collected.keys() == {"in"}
    assert merged.keys() == {"a", "b"}

    daemon.restart()
    client = daemon.connect()
    for target, card_path, expected in (("collect", "collect", collected),
                                        ("child", "child/merge", merged)):
        replay, status = run_and_wait(client, target, propagate=False)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        assert inputs(replay, card_path) == expected


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
    ended = []
    payloads = []
    for _ in range(2):
        run, status = run_and_wait(client, "start")
        assert status.state == RunState.succeeded, text(status.failure.diagnostic)
        value, = record(client, run, "child/relay").outputs
        payloads.append(bytes(value.inlineBytes))
        ended.append(status.endedAt)
    assert ended[0] < ended[1]
    deleted = client.call("RunHistoryDeleteRequest", endedBefore=ended[0] + 1,
                          opId=new_id())
    assert deleted.deletedRuns == 1

    daemon.restart()
    client = daemon.connect()
    for _ in range(2):
        run, status = run_and_wait(client, "child", propagate=False)
        assert status.state == RunState.succeeded, text(status.failure.diagnostic)
        value, = record(client, run, "child/relay").outputs
        assert text(value.rootType) == "Number"
        assert bytes(value.inlineBytes) == payloads[-1]
