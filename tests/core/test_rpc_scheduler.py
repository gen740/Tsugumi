"""Graph scheduling, opaque routing and terminal states through public RPC."""

import pytest

from harness.native_card import STATIC_RESULT_ENTRY, native_card
from harness.scenario import Project
from harness.values import reference_json
from projects import run_and_wait, wait_run, wait_run_notification
from tsugumi_client import new_id, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState

NUMBER = "table Number { value:long; } table Outputs { out:Number; } root_type Outputs;"
EMPTY = "table Outputs {} root_type Outputs;"
TEXT = "table Text { value:string; } table Outputs { out:Text; } root_type Outputs;"


def add_code(graph, directory, name, *, inputs=(), outputs=None, values=(), runs=None):
    packages = [native_card(directory / name, STATIC_RESULT_ENTRY, values)] if runs is None else runs
    return graph.card(name, {"type": "code", "recordIO": True,
                            "sandbox": {"filesystem": False},
                            "inputs": [{"name": name} for name in inputs]},
                      outputs=outputs or EMPTY,
                      plan={"product": {"runs": packages, "views": []},
                            "executionList": runs is not None})


def records(client, run, card):
    return client.call("RunInvocationsRequest", runId=run, card=path(card)).records or []


def record(client, run, card, invocation=1):
    return client.call("RunRecordRequest", runId=run, card=path(*card.split("/")), invocation=invocation).record


def collected(client, directory, run, card):
    value, = record(client, run, card).outputs
    return reference_json(directory, bytes(value.schemaBfbs), bytes(value.inlineBytes),
                          root_type="tsugumi.builtin.Collected")["items"]


def test_run_can_start_on_terminal_status_notification(core_factory, project):
    c = core_factory().connect()
    Project(project).card("start", {"type": "trigger"}).save(c)
    c.ensure_subscription()
    rev = c.call("ProjectInspectRequest").rev

    def start():
        result = c.call("RunRequest", baseRev=rev, opId=new_id(), origin="test",
                        card=path("start"), propagate=True)
        return text(result.run.runId)

    first = start()
    status = wait_run_notification(c, first)
    second = start()
    assert second != first
    assert status.state == RunState.succeeded
    assert wait_run(c, second).state == RunState.succeeded



def test_single_output_fans_out_and_only_connected_inputs_are_passed(core_factory, project, tmp_path):
    c = core_factory(mocked_builds=True).connect()
    payload = b"\xffopaque\0"
    graph = Project(project).card("start", {"type": "trigger"})
    add_code(graph, tmp_path, "src", outputs=NUMBER, values=[("out", payload)])
    add_code(graph, tmp_path, "left", inputs=["a", "unwired"])
    add_code(graph, tmp_path, "right", inputs=["b"])
    add_code(graph, tmp_path, "join", inputs=["a", "b"])
    graph.connect("src", "out", "left", "a").connect("src", "out", "right", "b")
    graph.connect("src", "out", "join", "a").connect("src", "out", "join", "b").save(c)

    run, status = run_and_wait(c, "start")
    assert status.state == RunState.succeeded, status.failure
    for card, port in [("left", "a"), ("right", "b")]:
        value, = record(c, run, card).inputs
        assert text(value.name) == port
        assert bytes(value.inlineBytes) == payload
    assert {text(v.name): bytes(v.inlineBytes) for v in record(c, run, "join").inputs} == {
        "a": payload, "b": payload,
    }


@pytest.mark.parametrize("absent", ["optional", "empty-list", "failed"])
def test_missing_output_skips_join_and_propagates(core_factory, project, tmp_path, absent):
    daemon = core_factory(mocked_builds=True)
    c = daemon.connect()
    graph = Project(project).card("start", {"type": "trigger"})
    add_code(graph, tmp_path, "arrives", outputs=NUMBER, values=[("out", b"arrived")])
    add_code(graph, tmp_path, "absent", outputs=NUMBER, runs=[] if absent == "empty-list" else None,
             values=[("unknown", b"bad")] if absent == "failed" else [])
    add_code(graph, tmp_path, "join", inputs=["left", "right"], outputs=NUMBER)
    add_code(graph, tmp_path, "downstream", inputs=["in"])
    graph.card("merge", {"type": "merge", "inputs": [{"name": "in"}]})
    graph.card("collect", {"type": "collect", "recordIO": True})
    graph.connect("arrives", "out", "join", "left").connect("absent", "out", "join", "right")
    graph.connect("join", "out", "merge", "in").connect("merge", "out", "downstream", "in")
    graph.connect("merge", "out", "collect", "in").save(c)

    run, status = run_and_wait(c, "start")
    failed = absent == "failed"
    assert status.state == (RunState.failed if failed else RunState.succeeded), status.failure
    if failed:
        assert status.failure.kind == FailureKind.output_invalid
    expected = CardState.blocked if failed else CardState.skipped
    for card in ("join", "merge", "downstream"):
        assert [row.state for row in records(c, run, card)] == [expected]
    if failed:
        assert [row.state for row in records(c, run, "collect")] == [CardState.blocked]
    else:
        assert collected(c, tmp_path / "decoded", run, "collect") == []
        daemon.restart()
        c = daemon.connect()
        assert [row.state for row in records(c, run, "downstream")] == [CardState.skipped]
        replay, status = run_and_wait(c, "collect", propagate=False)
        assert status.state == RunState.succeeded, status.failure
        assert collected(c, tmp_path / "replayed", replay, "collect") == []


def test_merge_forwards_heterogeneous_arrivals_unchanged(core_factory, project, tmp_path):
    c = core_factory(mocked_builds=True).connect()
    graph = Project(project).card("start", {"type": "trigger"})
    add_code(graph, tmp_path, "first", outputs=NUMBER, values=[("out", b"opaque number")])
    add_code(graph, tmp_path, "second", outputs=TEXT, values=[("out", b"opaque text")])
    add_code(graph, tmp_path, "absent", outputs=NUMBER)
    graph.card("merge", {"type": "merge", "recordIO": True, "maxRuns": 3,
                         "inputs": [{"name": n} for n in ("a", "b", "c", "unwired")]})
    graph.connect("first", "out", "merge", "a").connect("second", "out", "merge", "b")
    graph.connect("absent", "out", "merge", "c").save(c)

    run, status = run_and_wait(c, "start")
    assert status.state == RunState.succeeded, status.failure
    arrivals = set()
    for row in records(c, run, "merge"):
        assert row.state == CardState.succeeded
        invocation = record(c, run, "merge", row.invocation)
        incoming, = invocation.inputs
        outgoing, = invocation.outputs
        assert text(outgoing.name) == "out"
        assert bytes(incoming.schemaBfbs) == bytes(outgoing.schemaBfbs)
        assert bytes(incoming.inlineBytes) == bytes(outgoing.inlineBytes)
        arrivals.add((text(outgoing.rootType), bytes(outgoing.inlineBytes)))
    assert arrivals == {("Number", b"opaque number"), ("Text", b"opaque text")}


def test_collect_keeps_only_members_that_emit_the_port(core_factory, project, tmp_path):
    c = core_factory(mocked_builds=True).connect()
    graph = Project(project)
    packages = [
        native_card(tmp_path / "present", STATIC_RESULT_ENTRY, [("out", b"member")]),
        native_card(tmp_path / "empty", STATIC_RESULT_ENTRY),
    ]
    graph.card("start", {"type": "trigger"})
    add_code(graph, tmp_path, "batch", outputs=NUMBER, runs=packages)
    graph.card("collect", {"type": "collect", "recordIO": True})
    graph.connect("batch", "out", "collect", "in").save(c)

    run, status = run_and_wait(c, "start")
    assert status.state == RunState.succeeded, status.failure
    item, = collected(c, tmp_path / "decoded", run, "collect")
    assert bytes(item["bytes"]) == b"member"
    assert len(item["schema"]) == 64


def test_merge_still_enforces_max_runs(core_factory, project, tmp_path):
    c = core_factory(mocked_builds=True).connect()
    graph = Project(project)
    add_code(graph, tmp_path, "source", outputs=NUMBER, values=[("out", b"one")])
    graph.card("merge", {"type": "merge", "maxRuns": 1,
                         "inputs": [{"name": "a"}, {"name": "b"}]})
    graph.connect("source", "out", "merge", "a")
    graph.connect("source", "out", "merge", "b").save(c)

    run, status = run_and_wait(c, "source")
    assert status.state == RunState.failed
    assert status.failure.kind == FailureKind.execution_limit
    assert [row.state for row in records(c, run, "merge")] == [CardState.succeeded, CardState.failed]


def test_unwired_merge_skips_and_collect_emits_an_empty_value(core_factory, project, tmp_path):
    c = core_factory().connect()
    graph = Project(project).card("start", {"type": "trigger"})
    graph.card("merge", {"type": "merge", "inputs": [{"name": "unused"}]})
    graph.card("collect", {"type": "collect", "recordIO": True})
    graph.connect("merge", "out", "collect", "in").save(c)

    run, status = run_and_wait(c, "start")
    assert status.state == RunState.succeeded, status.failure
    assert [row.state for row in records(c, run, "merge")] == [CardState.skipped]
    assert collected(c, tmp_path / "decoded", run, "collect") == []
