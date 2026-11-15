"""ECONOMY: full evaluation counts, never elapsed time or build-cache flags.

The adapter counter observes NixRunner's full-evaluator boundary. These strict
xfails describe the pending content-keyed full-evaluation reuse in NixRunner;
they must be removed when that fix lands. The real-Nix correctness suite is
separate, so an adapter cannot stand in for evaluating a changed Nix expression.
"""

import json

import pytest

from harness.definition_evaluation import edit_json, full_evaluations, reload_project
from harness.fixtures import worker_project
from harness.observations import decode_scalar, read_output
from harness.scenario import run, wait_run
from tsugumi_client import change, new_id, path, text
from Tsugumi.Data.RunState import RunState
from Tsugumi.Data.Phase import Phase
from Tsugumi.Data.StageOutcome import StageOutcome


PENDING_REUSE = pytest.mark.xfail(
    strict=True, raises=AssertionError,
    reason="Pending fix: NixRunner must reuse full Card evaluation by derivation inputs across runs",
)


def completed(client, expected_calls=1):
    run_id = run(client)
    status = wait_run(client, run_id)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    assert decode_scalar(read_output(client, run_id)) == str(expected_calls)
    return run_id


@PENDING_REUSE
@pytest.mark.parametrize("irrelevant_edit", [
    "unchanged", "layout", "display-name", "description", "comment", "outside-file", "other-card",
    "runtime-param", "display-mode", "project-name", "language-label",
])
def test_non_derivation_edits_do_not_repeat_full_evaluation(
        core_factory, project, tmp_path, irrelevant_edit):
    """An unchanged derivation gets another execution without another full evaluation."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    worker_project(client, project, tmp_path,
                   [{"op": "output", "port": "out", "type": "string", "source": "calls"}],
                   reuseWorker=True, params=[{"name": "message", "type": "string", "data": "first"}])
    # Keep the second Card in the actual graph, without replacing work.
    snap = client.call("ProjectInspectRequest")
    doc = json.loads(text(snap.projectJson))
    doc["canvas"]["cards"].append({"id": "other"})
    client.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="user", changes=[
        change("SetProject", json=json.dumps(doc)),
        change("PutDefinition", id="other", cardJson='{"language":"bash","sandbox":false}'),
        change("PutSource", path="definitions/other/_outputs.fbs", bytes=b"table Outputs {} root_type Outputs;"),
        change("PutSource", path="definitions/other/default.nix",
               bytes=b'{ pkgs, inputs, params }: { run = "echo first"; }'),
    ])
    first = completed(client)
    baseline = full_evaluations(daemon)
    assert baseline == 1, "the counter must witness an initial full evaluation"
    if irrelevant_edit == "layout":
        (project / "layout.json").write_text('{"cards":{"work":{"x":100,"y":200}}}')
    elif irrelevant_edit in ("display-name", "description", "comment"):
        key, value = {
            "display-name": ("name", "Renamed"), "description": ("description", "Explanation"),
            "comment": ("comments", [{"text": "Observation", "author": "user"}]),
        }[irrelevant_edit]
        (project / "definitions/work/_meta.json").write_text(json.dumps({key: value}))
    elif irrelevant_edit == "outside-file":
        (project / "notes.txt").write_text("outside the Card definition")
    elif irrelevant_edit == "other-card":
        (project / "definitions/other/default.nix").write_text(
            '{ pkgs, inputs, params }: { run = "echo second"; }')
    elif irrelevant_edit == "runtime-param":
        edit_json(project / "definitions/work/_card.json", lambda card: card["params"][0].update(data="second"))
    elif irrelevant_edit == "display-mode":
        edit_json(project / "definitions/work/_card.json", lambda card: card.update(displayMode="live"))
    elif irrelevant_edit == "project-name":
        edit_json(project / "tsugumi.json", lambda doc: doc.update(name="Renamed project"))
    elif irrelevant_edit == "language-label":
        # The label is not a Nix argument; default.nix selects the implementation.
        edit_json(project / "definitions/work/_card.json", lambda card: card.update(language="python"))
    if irrelevant_edit != "unchanged":
        reload_project(client)
    second = completed(client, expected_calls=2)
    assert second != first, "definition reuse must still execute a new run"
    assert full_evaluations(daemon) == baseline


def test_full_evaluation_counter_counts_store_hits_and_changed_inputs(core_factory, project, tmp_path):
    """The observation counts evaluations, including cached products, rather than builds."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    worker_project(client, project, tmp_path,
                   [{"op": "output", "port": "out", "type": "string", "source": "calls"}])
    first = completed(client)
    assert full_evaluations(daemon) == 1
    # Force a store hit for a changed product: cached build outcomes must not
    # be confused with skipped evaluation. Only the adapter reads this plan.
    def change_product(plan):
        plan["manifest"]["preRun"] = "true"
        plan["cached"] = True
    edit_json(project / "definitions/work/mock-build.json", change_product)
    reload_project(client)
    second = completed(client)
    assert full_evaluations(daemon) == 2
    invocation, = client.call("RunInvocationsRequest", runId=second, card=path("work")).records
    stages = {(stage.phase, stage.outcome) for stage in invocation.stages}
    assert (Phase.evaluate, StageOutcome.done) in stages
    assert (Phase.build, StageOutcome.cached) in stages
    assert first != second
