"""Execution scenarios observed exclusively through the daemon's public RPC."""

import pytest

from harness.fixtures import add_worker, worker_project
from harness.observations import decode_scalar, read_output
from harness.scenario import STRING_OUTPUT, Gate, Project, run, wait_run
from tsugumi_client import path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState


@pytest.mark.parametrize("kind,schema_type,expected", [
    ("string", "string", "recorded 日本語"),
    ("int64", "long", -(2 ** 60)),
    ("uint64", "ulong", 2 ** 63 + 19),
    ("double", "double", 0.125),
    ("bool", "bool", True),
], ids=["utf8-string", "signed-64-bit", "unsigned-64-bit", "floating-point", "boolean"])
def test_worker_values_are_recorded_and_readable(core_factory, project, tmp_path, kind, schema_type, expected):
    """A completed worker's value is preserved in its public run record.

    Given a worker returning a declared scalar value,
    when its card runs to completion,
    then the value and saved run record both contain the original bytes.
    """
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    outputs = f"table Value {{ value:{schema_type}; }} table Outputs {{ out:Value (required); }} root_type Outputs;"
    worker_project(client, project, tmp_path,
                   [{"op": "output", "port": "out", "type": kind, "value": expected}], outputs=outputs)

    run_id = run(client)
    status = wait_run(client, run_id)
    assert status.state == RunState.succeeded
    payload = read_output(client, run_id)
    assert decode_scalar(payload, kind) == expected
    record = client.call("RunRecordRequest", runId=run_id, card=path("work"), invocation=1)
    assert record.exists and record.status.state == CardState.succeeded
    assert [text(output.name) for output in record.record.outputs] == ["out"]
    assert bytes(record.record.outputs[0].inlineBytes) == payload


@pytest.mark.parametrize("action,kind", [
    ({"op": "failure", "kind": "output_invalid", "message": "scripted rejection"}, FailureKind.output_invalid),
    ({"op": "exit", "code": 17}, FailureKind.worker_failed),
    ({"op": "malformed"}, FailureKind.worker_failed),
], ids=["worker-rejects-output", "worker-exits", "worker-sends-invalid-frame"])
def test_worker_failure_is_contained_in_the_run(core_factory, project, tmp_path, action, kind):
    """Worker failure is recorded while the daemon remains available.

    Given a worker that rejects its output, exits, or sends an invalid frame,
    when its card runs,
    then the run and card record report the failure and project inspection still works.
    """
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    worker_project(client, project, tmp_path, [action])

    run_id = run(client)
    status = wait_run(client, run_id)
    assert status.state == RunState.failed
    assert status.failure.kind == kind
    record = client.call("RunRecordRequest", runId=run_id, card=path("work"), invocation=1)
    assert record.status.state == CardState.failed
    assert record.status.failure.kind == kind
    assert client.call("ProjectInspectRequest").rev == 1


def test_gate_controls_a_downstream_worker_without_mocking_the_scheduler(core_factory, project, tmp_path):
    """A downstream card receives its upstream value and records its source.

    Given two connected cards and a downstream worker waiting at a gate,
    when the upstream card finishes and the downstream gate opens,
    then the downstream output matches its input and identifies the source card.
    """
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    gate = Gate(tmp_path, "downstream")
    graph = Project(project)
    add_worker(graph, "source", tmp_path / "source", [
        {"op": "output", "port": "out", "type": "string", "value": "through the graph"},
    ])
    add_worker(graph, "sink", tmp_path / "sink", [
        gate.action(),
        {"op": "echo", "input": "in", "port": "out"},
        {"op": "display_text", "text": "received input"},
    ], inputs=[{"name": "in"}], executionService="local")
    graph.connect("source", "out", "sink", "in").save(client)

    run_id = run(client, "source")
    gate.wait()
    source = client.call("RunRecordRequest", runId=run_id, card=path("source"))
    assert source.status.state == CardState.succeeded
    assert text(source.record.summary.executionService) == "local"
    sink = client.call("RunInvocationsRequest", runId=run_id, card=path("sink")).records[0]
    assert sink.state == CardState.running
    assert text(sink.executionService) == "local"
    gate.release()
    assert wait_run(client, run_id).state == RunState.succeeded
    assert read_output(client, run_id, card="sink") == read_output(client, run_id, card="source")
    record = client.call("RunRecordRequest", runId=run_id, card=path("sink"), invocation=1).record
    assert [text(segment) for segment in record.inputSources[0].card.segments] == ["source"]
    assert text(record.display.value.text) == "received input"


def test_cancel_a_worker_waiting_to_return(core_factory, project, tmp_path):
    """Cancelling a waiting worker records cancellation without publishing output.

    Given a worker paused before returning its value,
    when the user cancels the run,
    then the run and card are cancelled and no output is recorded.
    """
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    gate = Gate(tmp_path, "worker")
    worker_project(client, project, tmp_path, [gate.action(),
                   {"op": "output", "port": "out", "type": "string", "value": "too late"}])

    run_id = run(client)
    gate.wait()
    client.call("RunCancelRequest", runId=run_id)
    assert wait_run(client, run_id).state == RunState.cancelled
    record = client.call("RunRecordRequest", runId=run_id, card=path("work"), invocation=1)
    assert record.status.state == CardState.cancelled
    assert not record.record.outputs


def test_timeout_ends_a_worker_that_ignores_cancellation(core_factory, project, tmp_path):
    """A worker that ignores cancellation still reaches its run deadline.

    Given a hung worker that ignores cancellation and a one-second timeout,
    when the timeout expires,
    then the run terminates and its saved failure identifies the timeout.
    """
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    gate = Gate(tmp_path, "hung-worker")
    worker_project(client, project, tmp_path,
                   [{"op": "hang", "entered": str(gate.entered), "ignore_cancel": True}], timeoutSeconds=1)

    run_id = run(client)
    gate.wait()
    status = wait_run(client, run_id, timeout=6)
    assert status.state == RunState.failed
    assert status.failure.kind == FailureKind.timeout
    record = client.call("RunRecordRequest", runId=run_id, card=path("work"), invocation=1)
    assert record.status.failure.kind == FailureKind.timeout


def test_deadline_is_the_reason_when_a_cooperative_worker_reports_cancelled(core_factory, project, tmp_path):
    """A worker that stops when asked does not turn a deadline into a cancellation.

    Given a worker that answers the stop request by reporting cancelled,
    when its deadline expires,
    then the run and the Card record the timeout, the reason the host stopped it.
    """
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    gate = Gate(tmp_path, "cooperative-worker")
    worker_project(client, project, tmp_path, [{"op": "hang", "entered": str(gate.entered)}], timeoutSeconds=1)

    run_id = run(client)
    gate.wait()
    status = wait_run(client, run_id, timeout=6)
    assert status.state == RunState.failed
    assert status.failure.kind == FailureKind.timeout
    record = client.call("RunRecordRequest", runId=run_id, card=path("work"), invocation=1)
    assert record.status.failure.kind == FailureKind.timeout


@pytest.mark.parametrize("deadline", [True, False], ids=["deadline", "user-cancel"])
def test_a_failure_after_the_stop_request_keeps_its_text(core_factory, project, tmp_path, deadline):
    """A failure of the worker's own that races with a stop request stays readable.

    Given a worker that reports output_invalid just after it was asked to stop,
    when the stop came from the deadline or from the user,
    then the Card records why the host stopped it, and the diagnostic still carries the worker's failure.
    """
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    gate = Gate(tmp_path, "racing-worker")
    actions = [{"op": "failure_after_cancel", "entered": str(gate.entered),
                "kind": "output_invalid", "message": "scripted rejection"}]
    worker_project(client, project, tmp_path, actions, **({"timeoutSeconds": 1} if deadline else {}))

    run_id = run(client)
    gate.wait()
    if not deadline:
        client.call("RunCancelRequest", runId=run_id)
    status = wait_run(client, run_id, timeout=6)
    assert status.state == (RunState.failed if deadline else RunState.cancelled)
    failure = client.call("RunRecordRequest", runId=run_id, card=path("work"), invocation=1).status.failure
    assert failure.kind == (FailureKind.timeout if deadline else FailureKind.cancelled)
    assert "output_invalid: scripted rejection" in text(failure.diagnostic)


@pytest.mark.parametrize("cancel", [True, False], ids=["user-cancels-build", "build-deadline-expires"])
def test_build_wait_can_be_interrupted(core_factory, project, cancel):
    """A waiting build can be cancelled or stopped by its deadline.

    Given a build paused at a gate,
    when the user cancels it or its build deadline expires,
    then the run finishes with the corresponding cancellation or timeout state.
    """
    daemon = core_factory(mocked_builds=True)
    client = daemon.connect()
    gate = Gate(daemon.gates, "build")
    Project(project).card(
        "work", {"language": "bash", "sandbox": False, "buildTimeoutSeconds": 10 if cancel else 1},
        outputs=STRING_OUTPUT, manifest={"run": {"bash": "true"}}, plan={"gate": "build"},
    ).save(client)

    run_id = run(client)
    gate.wait()
    if cancel:
        client.call("RunCancelRequest", runId=run_id)
    status = wait_run(client, run_id, timeout=6)
    assert status.state == (RunState.cancelled if cancel else RunState.failed)
    if not cancel:
        assert status.failure.kind == FailureKind.timeout


def test_build_failure_becomes_an_inspectable_card_failure(core_factory, project):
    """A build error remains available in the card's run record.

    Given a build that cannot provide a dependency,
    when its card runs,
    then both the run and saved card failure identify the build error and diagnostic.
    """
    daemon = core_factory(mocked_builds=True)
    client = daemon.connect()
    Project(project).card("work", {"language": "bash", "sandbox": False},
                          outputs=STRING_OUTPUT, plan={"error": "dependency unavailable"}).save(client)
    run_id = run(client)
    status = wait_run(client, run_id)
    assert status.state == RunState.failed
    assert status.failure.kind == FailureKind.nix_failed
    record = client.call("RunRecordRequest", runId=run_id, card=path("work"), invocation=1)
    assert record.status.failure.kind == FailureKind.nix_failed
    assert "dependency unavailable" in text(record.status.failure.diagnostic)
