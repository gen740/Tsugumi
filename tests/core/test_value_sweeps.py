"""Implementation labels and Input value sweeps through the public RPC."""

import json

import pytest

from harness.fixtures import mock_plan
from harness.observations import read_output
from harness.scenario import Project, run, wait_run
from harness.values import recorded_input_json, reference_json
from projects import write
from tsugumi_client import path, text
from Tsugumi.Data.ExecutionShape import ExecutionShape
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState


NUMBER = 'table Number { value:ulong; } table Outputs { out:Number (required); } root_type Outputs;'
PAIR = 'table Pair { left:ulong; right:ulong; } table Outputs { out:Pair (required); } root_type Outputs;'
VECTOR = 'table Vector { items:[ulong]; } table Outputs { out:Vector (required); } root_type Outputs;'


def numeric_plan(directory, values, **options):
    runs = []
    for value in values:
        package = directory / f"number-{value}"
        package.mkdir()
        (package / "scenario.json").write_text(json.dumps({
            "actions": [{"op": "output", "port": "out", "type": "uint64", "value": value}],
        }))
        runs.append(str(package))
    return mock_plan(runs, execution_list=True, **options)


def invocation_page(client, run_id, card):
    return client.call("RunInvocationsRequest", runId=run_id, card=path(card))


def collected(client, directory, run_id, card="collect"):
    record = client.call("RunRecordRequest", runId=run_id, card=path(card)).record
    value, = record.outputs
    payload = read_output(client, run_id, card=card)
    return reference_json(directory, bytes(value.schemaBfbs), payload,
                          root_type=text(value.rootType))["items"]


def label(row):
    return text(row.implementationLabel) if row.implementationLabel is not None else None


def test_run_list_labels_name_each_invocation(core_factory, project, tmp_path):
    """Labels from the execution plan are recorded on every member, and an unlabeled entry keeps only its index."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    graph = Project(project)
    graph.card("work", {"type": "code", "sandbox": {"filesystem": False}, "recordIO": True}, outputs=NUMBER,
               plan=numeric_plan(tmp_path, [1, 2, 3], labels=["threshold 10", "", "threshold 14"]))
    graph.save(client)

    run_id = run(client, "work", propagate=False)
    assert wait_run(client, run_id).state == RunState.succeeded
    page = invocation_page(client, run_id, "work")
    assert [(row.implementationIndex, label(row)) for row in page.records] == [
        (1, "threshold 10"), (2, None), (3, "threshold 14")]
    latest, = [row for row in client.call("RunLatestInvocationsRequest", runId=run_id).records
               if row.card.segments == [b"work"]]
    assert label(latest) == "threshold 14"
    record = client.call("RunRecordRequest", runId=run_id, card=path("work"), invocation=1).record
    assert label(record.summary) == "threshold 10"


@pytest.mark.parametrize("labels,message", [
    (["same", "same"], "names more than one implementation"),
    (["tab\there", ""], "control character"),
])
def test_invalid_run_list_labels_fail_before_any_member_runs(core_factory, project, tmp_path, labels, message):
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    graph = Project(project).card("work", {"type": "code", "sandbox": {"filesystem": False}}, outputs=NUMBER,
                                  plan=numeric_plan(tmp_path, [1, 2], labels=labels))
    graph.save(client)

    run_id = run(client, "work", propagate=False)
    status = wait_run(client, run_id)
    assert status.state == RunState.failed
    assert message in text(status.failure.diagnostic)
    assert all(row.batchIndex is None for row in invocation_page(client, run_id, "work").records)


def test_input_value_sweep_runs_the_cartesian_product_of_its_values(core_factory, project, tmp_path):
    """Each swept value is one member; the last param varies fastest and arriving batches stay the outer axis."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    graph = Project(project)
    graph.card("starts", {"type": "code", "sandbox": {"filesystem": False}}, outputs=NUMBER,
               plan=numeric_plan(tmp_path, [7, 8]))
    graph.card("values", {"type": "input", "recordIO": True, "params": [
        {"name": "left", "type": "uint64", "sweep": [1, 2]},
        {"name": "right", "type": "uint64", "sweep": [10, 20, 30]},
    ]}, outputs=PAIR)
    graph.card("collect", {"type": "collect", "recordIO": True})
    graph.connect("starts", "out", "values", "start").connect("values", "out", "collect", "in")
    graph.save(client)

    run_id = run(client, "starts", propagate=True)
    assert wait_run(client, run_id).state == RunState.succeeded
    page = invocation_page(client, run_id, "values")
    batch, = page.batches
    assert batch.executionList and batch.complete and not batch.failed and batch.invocationCount == 12
    pairs = [(left, right) for left in (1, 2) for right in (10, 20, 30)]
    assert [label(row) for row in page.records] == [f"left={a}, right={b}" for a, b in pairs] * 2
    assert [row.implementationIndex for row in page.records] == list(range(1, 7)) * 2
    assert [row.batchIndex for row in page.records] == list(range(1, 13))
    first = client.call("RunRecordRequest", runId=run_id, card=path("values"), invocation=2).record
    assert json.loads(text(first.paramsJson)) == {"left": 1, "right": 20}
    assert recorded_input_json(client, tmp_path / "decode", run_id, "values", invocation=7, port="start") == {
        "value": 8}
    assert collected(client, tmp_path / "decode", run_id) == [{"left": a, "right": b} for a, b in pairs] * 2


def test_singleton_sweep_is_a_batch_and_a_list_param_is_one_vector(core_factory, project, tmp_path):
    """sweep: [x] keeps an explicit one-member batch; data holding a list stays one ordinary value."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    graph = Project(project)
    graph.card("one", {"type": "input", "recordIO": True,
                       "params": [{"name": "value", "type": "uint64", "sweep": [5]}]}, outputs=NUMBER)
    graph.card("vector", {"type": "input", "recordIO": True,
                          "params": [{"name": "items", "type": "uint64[]", "data": [1, 2, 3]}]}, outputs=VECTOR)
    graph.card("lists", {"type": "input", "recordIO": True,
                         "params": [{"name": "items", "type": "uint64[]", "sweep": [[1, 2], [], [3]]}]},
               outputs=VECTOR)
    for name in ("one", "vector", "lists"):
        graph.card(f"collect-{name}", {"type": "collect", "recordIO": True})
        graph.connect(name, "out", f"collect-{name}", "in")
    graph.save(client)

    shapes = {"/".join(text(s) for s in node.card.segments): node.outputs[0].executionShape
              for node in client.call("ProjectInspectRequest").resolution if node.outputs}
    assert shapes["one"] == shapes["lists"] == ExecutionShape.list
    assert shapes["vector"] == ExecutionShape.single

    results = {}
    for name in ("one", "vector", "lists"):
        run_id = run(client, name, propagate=True)
        assert wait_run(client, run_id).state == RunState.succeeded
        batches = invocation_page(client, run_id, name).batches or []
        results[name] = ([(b.executionList, b.invocationCount) for b in batches],
                         collected(client, tmp_path / "decode", run_id, f"collect-{name}"))
    assert results["one"] == ([(True, 1)], [{"value": 5}])
    assert results["vector"][1] == [{"items": [1, 2, 3]}]
    assert all(not execution_list for execution_list, _ in results["vector"][0])
    assert results["lists"] == ([(True, 3)], [{"items": [1, 2]}, {"items": []}, {"items": [3]}])


def test_empty_sweep_sends_an_empty_batch(core_factory, project, tmp_path):
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    graph = Project(project)
    graph.card("values", {"type": "input", "params": [{"name": "value", "type": "uint64", "sweep": []}]},
               outputs=NUMBER)
    graph.card("collect", {"type": "collect", "recordIO": True})
    graph.connect("values", "out", "collect", "in").save(client)

    run_id = run(client, "values", propagate=True)
    assert wait_run(client, run_id).state == RunState.succeeded
    batch, = invocation_page(client, run_id, "values").batches
    assert batch.executionList and batch.invocationCount == 0
    assert {text(o.port): (o.count, o.executionList) for o in batch.outputs} == {"out": (0, True)}
    assert collected(client, tmp_path / "decode", run_id) == []


def test_sweep_and_arriving_batch_share_the_combination_limit(core_factory, project, tmp_path):
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    graph = Project(project)
    graph.card("starts", {"type": "code", "sandbox": {"filesystem": False}}, outputs=NUMBER,
               plan=numeric_plan(tmp_path, [0, 1]))
    graph.card("values", {"type": "input", "params": [
        {"name": "value", "type": "uint64", "sweep": list(range(200))}]}, outputs=NUMBER)
    graph.connect("starts", "out", "values", "start").save(client)

    run_id = run(client, "starts", propagate=True)
    status = wait_run(client, run_id)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.execution_limit
    assert "256 combinations" in text(status.failure.diagnostic)
    batch, = invocation_page(client, run_id, "values").batches
    assert batch.failed and not batch.outputs


@pytest.mark.parametrize("definition,message", [
    ({"type": "input", "params": [{"name": "value", "type": "uint64", "data": 1, "sweep": [1]}]},
     "not both"),
    ({"type": "input", "params": [{"name": "value", "type": "uint64", "sweep": [1, -2]}]},
     "sweep[1]: expected uint64"),
    ({"type": "input", "params": [{"name": "left", "type": "uint64", "sweep": list(range(17))},
                                      {"name": "right", "type": "uint64", "sweep": list(range(16))}]},
     "at most 256"),
    ({"type": "code", "params": [{"name": "value", "type": "uint64", "sweep": [1, 2]}],
      "_nix": "{ pkgs, inputs, params }: { run = pkgs.emptyDirectory; }"},
     "sweep is for Input Cards"),
])
def test_invalid_sweeps_are_definition_errors(core_factory, project, definition, message):
    write(project, {"values": {**definition, "_outputs": NUMBER}}, [{"id": "values", "definition": "values"}], [])
    client = core_factory(project_root=project).connect()
    node, = [node for node in client.call("ProjectInspectRequest").resolution
             if node.card.segments == [b"values"]]
    assert any(text(d.code) == "invalid_param" and message in text(d.message) for d in node.diagnostics), [
        text(d.message) for d in node.diagnostics]
