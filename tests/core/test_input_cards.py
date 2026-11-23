"""Build plan labels and Input Card outputs through the public RPC."""

import json

import pytest

from harness.fixtures import mock_plan
from harness.scenario import Project, run, wait_run
from harness.observations import read_output
from harness.values import encode_message
from tsugumi_client import path, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState


NUMBER = 'table Number { value:ulong; } table Outputs { out:Number (required); } root_type Outputs;'


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


def test_input_card_emits_built_variants_without_a_worker(core_factory, project, tmp_path):
    store = tmp_path / "store"
    client = core_factory(mocked_builds=True, env={"TSUGUMI_MOCK_STORE": str(store)}).connect()
    values = [17, 23]
    packages, payloads = [], []
    for value in values:
        package = store / f"number-{value}"
        port = package / "share/tsugumi/input/ports/out"
        port.parent.mkdir(parents=True)
        payload = encode_message(tmp_path / f"encode-{value}", NUMBER, {"value": value}, root_type="Number")
        port.write_bytes(payload)
        packages.append(str(package))
        payloads.append(payload)
    graph = Project(project).card("work", {"type": "input", "recordIO": True,
                                         "_params": "{ values = [ 17 23 ]; }"},
                                  outputs=NUMBER,
                                  plan=mock_plan(packages, execution_list=True, labels=["17", "23"]))
    graph.save(client)
    run_id = run(client, "work", propagate=False)
    status = wait_run(client, run_id)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    page = invocation_page(client, run_id, "work")
    assert [(row.implementationIndex, label(row)) for row in page.records] == [
        (1, "17"), (2, "23")]
    for invocation, payload in enumerate(payloads, 1):
        assert read_output(client, run_id, card="work", invocation=invocation) == payload


@pytest.mark.parametrize("required", [True, False])
def test_input_card_missing_output_follows_required_attribute(core_factory, project, tmp_path, required):
    store = tmp_path / "store"
    package = store / "empty"
    package.mkdir(parents=True)
    client = core_factory(mocked_builds=True, env={"TSUGUMI_MOCK_STORE": str(store)}).connect()
    schema = NUMBER if required else NUMBER.replace(" (required)", "")
    Project(project).card("work", {"type": "input"}, outputs=schema,
                          plan=mock_plan([str(package)])).save(client)
    status = wait_run(client, run(client, "work", propagate=False))
    if required:
        assert status.state == RunState.failed
        assert status.failure.kind == FailureKind.output_invalid
        assert "required output out is missing" in text(status.failure.diagnostic)
    else:
        assert status.state == RunState.succeeded
