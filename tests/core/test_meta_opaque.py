"""The project RPC carries presentation bytes; policy lives in agentd."""
import json

import pytest

from harness.fixtures import worker_project
from harness.scenario import Gate, run, wait_run
from projects import builtin_project, card
from tsugumi_client import RpcFailure, change, new_id, path, text
from Tsugumi.Data.RunState import RunState


def save(client, changes):
    return client.call("ProjectSaveRequest", baseRev=client.call("ProjectInspectRequest").rev,
                       opId=new_id(), origin="agent", changes=changes)


def test_direct_rpc_does_not_stamp_comments_or_enforce_draft_intent(tmp_path, core_factory):
    root = tmp_path / "p"
    builtin_project(root, extra_cards=[card("plan")])
    (root / "definitions/plan/_meta.json").write_text('{"description":"Original","content":"Plan"}')
    client = core_factory(project_root=root).connect()
    meta = '{"future":true,"name":false,"description":[],"comments":[{"text":"Ask","author":"user"}]}'
    save(client, [change("PutDefinition", id="plan", cardJson='{"template":"sink"}', metaJson=meta)])
    result = next(d for d in client.call("ProjectInspectRequest").definitions if text(d.id) == "plan")
    assert text(result.metaJson) == meta
    for invalid in ('[]', 'null', '{'):
        with pytest.raises(RpcFailure):
            save(client, [change("PutDefinition", id="plan", cardJson='{"template":"sink"}', metaJson=invalid)])


def test_presentation_saves_while_busy_keep_execution_revision_and_run_source_hash(core_factory, project, tmp_path):
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    gate = Gate(tmp_path, "active")
    worker_project(client, project, tmp_path, [gate.action(), {"op":"output", "port":"out", "type":"string", "value":"done"}])
    before = client.call("ProjectInspectRequest")
    run_id = run(client)
    gate.wait()
    try:
        document = json.loads(text(before.projectJson))
        document["name"] = {"future": "Renamed"}
        definition = next(d for d in before.definitions if text(d.id) == "work")
        settings = json.loads(text(definition.cardJson))
        settings["displayMode"] = {"future": "anything"}
        result = save(client, [change("SetProject", json=json.dumps(document)),
            change("PutDefinition", id="work", cardJson=json.dumps(settings), metaJson='{"comments":false,"future":12}')])
        assert result.executionRev == before.executionRev
        settings["maxRuns"] = 2
        with pytest.raises(RpcFailure) as failure:
            save(client, [change("PutDefinition", id="work", cardJson=json.dumps(settings), keepMeta=True)])
        assert failure.value.kind == "busy"
    finally:
        gate.release()
    assert wait_run(client, run_id).state == RunState.succeeded
    second = run(client)
    assert wait_run(client, second).state == RunState.succeeded
    summaries = {text(r.status.runId): r for r in client.call("RunHistoryRequest").runs}
    assert text(summaries[run_id].sourceHash) == text(summaries[second].sourceHash)
    assert client.call("RunRecordRequest", runId=run_id, card=path("work")).record
