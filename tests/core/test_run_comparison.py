"""What comparing two retained runs reads from core, checked through public RPC:
the parameters each invocation ran with, whatever its outcome, the digest that
tells two recorded values apart, and both surviving a copy of the project."""

import json
import shutil

from harness.fixtures import add_worker
from harness.scenario import Gate, Project, run, wait_run
from tsugumi_client import change, new_id, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.RunState import RunState


def param(name, value):
    return {"name": name, "type": "uint64", "data": value}


def recorded_params(client, run_id, card):
    result = client.call("RunRecordRequest", runId=run_id, card=path(card))
    return result, json.loads(text(result.record.paramsJson))


def set_param(client, card, name, value):
    snapshot = client.call("ProjectInspectRequest")
    definition = next(d for d in snapshot.definitions if text(d.id) == card)
    body = json.loads(text(definition.cardJson))
    next(p for p in body["params"] if p["name"] == name)["data"] = value
    client.call("ProjectSaveRequest", baseRev=snapshot.rev, opId=new_id(), origin="ui:test",
                changes=[change("PutDefinition", id=card, cardJson=json.dumps(body))])


def test_parameters_are_recorded_for_failed_cancelled_and_never_started_cards(core_factory, project, tmp_path):
    """A Card's parameters are in its record however the invocation ended.

    Given a Card that fails, the Card after it that never starts, and a Card
    stopped while it waits,
    when the runs end and the document's parameters are edited afterwards,
    then each record still holds the parameters that run was started with.
    """
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    gate = Gate(tmp_path, "stopped")
    graph = Project(project)
    add_worker(graph, "fails", tmp_path / "fails", [{"op": "failure", "message": "the band is empty"}],
               params=[param("boundary", 1650)])
    add_worker(graph, "after", tmp_path / "after", [{"op": "echo", "input": "in", "port": "out"}],
               inputs=[{"name": "in"}], params=[param("bins", 64)])
    add_worker(graph, "stopped", tmp_path / "stopped", [
        gate.action(), {"op": "output", "port": "out", "type": "string", "value": "too late"},
    ], params=[param("low", 600)])
    graph.connect("fails", "out", "after", "in").save(client)

    failed_run = run(client, "fails")
    assert wait_run(client, failed_run).state == RunState.failed
    cancelled_run = run(client, "stopped")
    gate.wait()
    client.call("RunCancelRequest", runId=cancelled_run)
    assert wait_run(client, cancelled_run).state == RunState.cancelled

    # The document moves on; the records must not follow it.
    set_param(client, "fails", "boundary", 1800)
    set_param(client, "after", "bins", 128)
    set_param(client, "stopped", "low", 700)
    repeated_run = run(client, "fails")
    assert wait_run(client, repeated_run).state == RunState.failed

    def check(c):
        result, params = recorded_params(c, failed_run, "fails")
        assert result.status.state == CardState.failed and params == {"boundary": 1650}
        result, params = recorded_params(c, repeated_run, "fails")
        assert result.status.state == CardState.failed and params == {"boundary": 1800}
        result, params = recorded_params(c, failed_run, "after")
        assert result.status.state in (CardState.blocked, CardState.skipped), "after never started"
        assert params == {"bins": 64}
        result, params = recorded_params(c, cancelled_run, "stopped")
        assert result.status.state == CardState.cancelled and params == {"low": 600}

    check(client)
    daemon.restart()
    check(daemon.connect())


def test_recorded_values_carry_a_content_digest_that_tells_runs_apart(core_factory, project, tmp_path):
    """Two recorded values compare by digest without downloading their bytes.

    Given Cards that send equal and different values, small and too large to inline,
    when they run, twice,
    then equal content has one digest whichever Card, port side or run recorded it,
    different content has another, and a value left out of the reply still has its digest.
    """
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    graph = Project(project)
    large = 300 << 10  # above what a record returns inline
    values = {"one": "1650", "same": "1650", "other": "1800",
              "big": "a" * large, "big_same": "a" * large, "big_other": "b" * large}
    # One chain (an output feeds one input): one -> sink -> same -> other -> big -> ...
    order = ["one", "sink", *[name for name in values if name != "one"]]
    for name in order:
        actions = ([{"op": "echo", "input": "in", "port": "out"}] if name == "sink" else
                   [{"op": "output", "port": "out", "type": "string", "value": values[name]}])
        add_worker(graph, name, tmp_path / name, actions, **({} if name == "one" else {"inputs": [{"name": "in"}]}))
    for upstream, downstream in zip(order, order[1:]):
        graph.connect(upstream, "out", downstream, "in")
    graph.save(client)

    def digests():
        run_id = run(client, "one")
        assert wait_run(client, run_id).state == RunState.succeeded
        out = {}
        for name in values:
            result = client.call("RunRecordRequest", runId=run_id, card=path(name))
            value = result.record.outputs[0]
            assert len(text(value.contentHash)) == 64
            if name.startswith("big"):
                assert result.truncated and not value.inlineBytes and value.sizeBytes > large
            out[name] = text(value.contentHash)
        received = client.call("RunRecordRequest", runId=run_id, card=path("sink")).record.inputs[0]
        assert text(received.contentHash) == out["one"], "the value sent is the value received"
        return out

    first, second = digests(), digests()
    assert first == second, "equal content, equal digest, whichever run recorded it"
    assert first["one"] == first["same"] != first["other"]
    assert first["big"] == first["big_same"] != first["big_other"]


def test_a_project_copy_keeps_recorded_invocation_details(core_factory, project, tmp_path):
    """The execution service, parameters and digests travel with project history.

    Given a run recorded in a project that saves its run history,
    when the project folder is copied and opened with an empty history disk,
    then the restored record has the same execution service, parameters and digest.
    """
    original = core_factory(mocked_builds=True, mocked_worker=True, history_dir=tmp_path / "history-original")
    client = original.connect()
    graph = Project(project)
    add_worker(graph, "work", tmp_path / "package", [
        {"op": "output", "port": "out", "type": "string", "value": "counts"},
    ], params=[param("boundary", 1650)])
    graph.save(client)
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    result, params = recorded_params(client, run_id, "work")
    digest = text(result.record.outputs[0].contentHash)
    assert params == {"boundary": 1650} and digest
    set_param(client, "work", "boundary", 1800)
    original.stop()

    copied_project = tmp_path / "copy"
    shutil.copytree(project, copied_project, symlinks=True)
    assert (copied_project / ".tsugumi" / "runs.sqlite").exists()
    copied = core_factory(project_root=copied_project, mocked_builds=True, mocked_worker=True,
                          history_dir=tmp_path / "history-copy")
    result, params = recorded_params(copied.connect(), run_id, "work")
    assert params == {"boundary": 1650}, "the parameters of the run, not of the copied document"
    assert text(result.record.outputs[0].contentHash) == digest
    assert text(result.status.executionService) == "local"
