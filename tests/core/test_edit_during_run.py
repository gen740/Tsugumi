"""Saving while a run is active: the run keeps the revision it started with.

A run executes the document it was accepted with. Each Card is evaluated from
its definition folder when the run reaches it, so only those files stay
fixed until the run ends; every other edit is saved and used by the next run.
"""

import json

import pytest

from harness.document import card_document, set_param
from harness.fixtures import add_worker
from harness.scenario import Gate, Project, run, wait_run
from tsugumi_client import RpcFailure, change, new_id, path, text
from Tsugumi.Data.RunState import RunState


def build(project, tmp_path, gate):
    graph = Project(project)
    add_worker(graph, "first", tmp_path / "first",
               [gate.action(), {"op": "output", "port": "out", "type": "string", "value": "one"}])
    add_worker(graph, "second", tmp_path / "second", [{"op": "echo", "input": "in", "port": "out"}],
               inputs=[{"name": "in"}], params={"bins": 0})
    add_worker(graph, "apart", tmp_path / "apart",
               [{"op": "output", "port": "out", "type": "string", "value": "apart"}], params={"bins": 64})
    return graph.connect("first", "out", "second", "in")


def put_settings(client, card, **settings):
    snap = client.call("ProjectInspectRequest")
    definition = next(d for d in snap.definitions if text(d.id) == card)
    document = json.loads(text(definition.cardJson))
    document.update(settings)
    return client.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="agent", changes=[
        change("PutDefinition", id=card, cardJson=json.dumps(card_document(document)),
               keepReadme=True, keepComments=True)])


def record(client, run_id, card):
    return client.call("RunRecordRequest", runId=run_id, card=path(card)).record


def test_an_edit_during_a_run_is_saved_and_used_by_the_next_run(core_factory, project, tmp_path):
    """Given a run waiting in its first Card, with a later Card not yet evaluated,
    when the later Card's settings and a Card outside the run are changed,
    then both saves succeed, the run finishes with the revision it started with,
    and the next run uses the new one."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    gate = Gate(tmp_path, "first")
    build(project, tmp_path, gate).save(client)
    before = client.call("ProjectInspectRequest")
    active = run(client, "first")
    gate.wait()
    try:
        # A computational setting of a Card the run has not evaluated yet.
        saved = put_settings(client, "second", recordIO="off")
        assert saved.executionRev == before.executionRev + 1
        # A Card the run does not read at all.
        set_param(client, "apart", "bins", 128)
    finally:
        gate.release()
    status = wait_run(client, active)
    assert status.state == RunState.succeeded
    assert status.executionRev == before.executionRev
    assert record(client, active, "second").recordIo

    following = run(client, "first")
    status = wait_run(client, following)
    assert status.state == RunState.succeeded
    assert status.executionRev == client.call("ProjectInspectRequest").executionRev
    assert not record(client, following, "second").recordIo
    separate = run(client, "apart")
    assert wait_run(client, separate).state == RunState.succeeded
    params = {text(p.name): json.loads(text(p.value))
              for p in record(client, separate, "apart").params.entries}
    assert params == {"bins": 128}


def test_a_definition_the_run_still_reads_stays_until_it_ends(core_factory, project, tmp_path):
    """Given a run waiting in its first Card,
    when a later Card's source (here its parameters) is changed,
    then the save is refused with what to do, names the Card and the run, and
    succeeds once the run has ended."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    gate = Gate(tmp_path, "first")
    build(project, tmp_path, gate).save(client)
    active = run(client, "first")
    gate.wait()
    try:
        with pytest.raises(RpcFailure) as refused:
            set_param(client, "second", "bins", 2)
        assert refused.value.kind == "busy"
        assert "definition of second" in refused.value.diagnostic
        assert "after the run ends" in refused.value.diagnostic
        assert refused.value.args_map["card"] == "second"
        assert refused.value.args_map["run_id"] == active
    finally:
        gate.release()
    assert wait_run(client, active).state == RunState.succeeded
    set_param(client, "second", "bins", 2)
    following = run(client, "first")
    assert wait_run(client, following).state == RunState.succeeded
    params = {text(p.name): json.loads(text(p.value))
              for p in record(client, following, "second").params.entries}
    assert params == {"bins": 2}


def test_description_and_comment_edits_do_not_disturb_a_later_card(core_factory, project, tmp_path):
    """A Card's README and comments are the document's, not its evaluation's:
    changing them before the run reaches the Card leaves the run unchanged."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    gate = Gate(tmp_path, "first")
    build(project, tmp_path, gate).save(client)
    active = run(client, "first")
    gate.wait()
    try:
        snap = client.call("ProjectInspectRequest")
        definition = next(d for d in snap.definitions if text(d.id) == "second")
        client.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="ui:test", changes=[
            change("PutDefinition", id="second", cardJson=text(definition.cardJson),
                   readme="---\ntitle: second\ndescription: Changed during the run\n---\n",
                   commentsJson=json.dumps({"comments": [{"text": "Look at this", "author": "user"}]}))])
    finally:
        gate.release()
    assert wait_run(client, active).state == RunState.succeeded
