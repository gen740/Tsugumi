"""Exercise Cartesian batches, collection and history through the public RPC."""

from itertools import product
import json
import shutil

import pytest

from projects import wait_invocation
from harness.fixtures import mock_plan
from harness.observations import read_output
from harness.scenario import Gate, Project, run, wait_run
from harness.values import collected_items as collected, encode_message, recorded_input_json, reference_json
from tsugumi_client import path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.RPC.RunEventValue import RunEventValue


NUMBER = 'table Number { value:ulong; } table Outputs { out:Number (required); } root_type Outputs;'
ARTIFACT_OUTPUT = 'table Doc {} table Outputs { out:Doc; } root_type Outputs;'


def scripted_implementations(directory, scenarios):
    directory.mkdir(parents=True, exist_ok=True)
    implementations = []
    for index, actions in enumerate(scenarios):
        package = directory / f"package-{index}"
        package.mkdir(exist_ok=True)
        (package / "scenario.json").write_text(json.dumps({"actions": actions}))
        implementations.append(str(package))
    return implementations


def numeric_implementations(directory, values):
    return scripted_implementations(directory, [
        [{"op": "output", "port": "out", "type": "uint64", "value": value}]
        for value in values
    ])


def producer(graph, name, values):
    runs = numeric_implementations(graph.directory.parent / ("runs-" + name), values)
    graph.card(name, {"type": "code", "sandbox": {"filesystem": False}}, outputs=NUMBER,
               plan=mock_plan(runs, execution_list=True))


def consumer(graph, *, implementations=None):
    runs = numeric_implementations(graph.directory.parent / "runs-work", [99]) if implementations is None else implementations
    graph.card("work", {"type": "code", "sandbox": {"filesystem": False}, "recordIO": "on", "maxRuns": 1,
                        "inputs": [{"name": "left"}, {"name": "right"}]},
               outputs=NUMBER, plan=mock_plan(runs, execution_list=implementations is not None))
    graph.connect("left", "out", "work", "left").connect("right", "out", "work", "right")


def connect_collect(graph):
    graph.card("collect", {"type": "collect", "recordIO": "on"})
    graph.connect("work", "out", "collect", "in")


def invocation_page(client, run_id, card):
    return client.call("RunInvocationsRequest", runId=run_id, card=path(card))


def label(row):
    return text(row.implementationLabel) if row.implementationLabel is not None else None


def test_run_list_labels_name_each_invocation(core_factory, project, tmp_path):
    """Labels from the execution plan are recorded on every member, and an unlabeled entry keeps only its index."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    graph = Project(project)
    graph.card("work", {"type": "code", "sandbox": {"filesystem": False}, "recordIO": "on"}, outputs=NUMBER,
               plan=mock_plan(numeric_implementations(tmp_path, [1, 2, 3]), execution_list=True, labels=["threshold 10", "", "threshold 14"]))
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
                                  plan=mock_plan(numeric_implementations(tmp_path, [1, 2]), execution_list=True, labels=labels))
    graph.save(client)

    run_id = run(client, "work", propagate=False)
    status = wait_run(client, run_id)
    assert status.state == RunState.failed
    assert message in text(status.failure.diagnostic)
    assert all(row.batchIndex is None for row in invocation_page(client, run_id, "work").records)


def combinations(client, directory, run_id):
    page = invocation_page(client, run_id, "work")
    result = []
    for row in page.records:
        left = recorded_input_json(client, directory, run_id, "work", invocation=row.invocation, port="left")
        right = recorded_input_json(client, directory, run_id, "work", invocation=row.invocation, port="right")
        result.append((left["value"], right["value"], row.implementationIndex))
    return page, result


def test_cartesian_order_is_saved_and_replayed(core_factory, project, tmp_path):
    """One trigger runs the complete Cartesian product and the saved inputs replay it in order."""
    history_dir = tmp_path / "history"
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    graph = Project(project).card("start", {"type": "trigger"})
    producer(graph, "left", [10, 20])
    producer(graph, "right", [1, 2])
    consumer(graph, implementations=numeric_implementations(tmp_path, [101, 202]))
    connect_collect(graph)
    graph.save(client)
    client.open()

    run_id = run(client, "start", propagate=True)
    assert wait_run(client, run_id).state == RunState.succeeded

    def work_batch(note):
        if note.valueType != NotificationPayload.RunEvent:
            return False
        event = note.value
        return (text(event.runId) == run_id and event.valueType == RunEventValue.RunBatchChanged
                and event.value.batch.card.segments == [b"work"])

    started = client.wait_notification(work_batch).value.value.batch
    finished = client.wait_notification(work_batch).value.value.batch
    assert started.activation == finished.activation == 1
    assert started.invocationCount == finished.invocationCount == 8
    assert not started.complete and finished.complete and not finished.failed

    expected = list(product([10, 20], [1, 2], [1, 2]))
    page, actual = combinations(client, tmp_path / "decode", run_id)
    assert actual == expected
    assert [row.batchIndex for row in page.records] == list(range(1, 9))
    assert all(row.batchActivation == 1 and row.batchSize == 8 for row in page.records)
    batch, = page.batches
    assert batch.complete and not batch.failed and batch.executionList and batch.invocationCount == 8
    assert collected(client, tmp_path / "decode", run_id) == [{"value": 101}, {"value": 202}] * 4

    daemon.restart()
    client = daemon.connect()
    saved, = invocation_page(client, run_id, "work").batches
    assert saved.complete and saved.invocationCount == 8
    replay = run(client, "work", propagate=False)
    assert wait_run(client, replay).state == RunState.succeeded
    assert combinations(client, tmp_path / "decode", replay)[1] == expected

    daemon.stop()
    copied = tmp_path / "copied-project"
    shutil.copytree(project, copied, symlinks=True)
    restored = core_factory(project_root=copied).connect()
    batch, = invocation_page(restored, run_id, "work").batches
    assert batch.complete and batch.invocationCount == 8
    assert collected(restored, tmp_path / "decode", run_id) == [{"value": 101}, {"value": 202}] * 4


def test_empty_batch_is_delivered_to_collect_without_starting_a_worker(core_factory, project, tmp_path):
    """An empty run list skips the join and Collect emits an empty vector."""
    diagnostic = "the evaluation selected no implementations"
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    graph = Project(project).card("start", {"type": "trigger"})
    graph.card("left", {"type": "code", "sandbox": {"filesystem": False}}, outputs=NUMBER,
               plan=mock_plan([], execution_list=True, log=diagnostic))
    producer(graph, "right", [1])
    consumer(graph)
    connect_collect(graph)
    graph.save(client)

    client.open()

    run_id = run(client, "start", propagate=True)
    assert wait_run(client, run_id).state == RunState.succeeded
    page = invocation_page(client, run_id, "left")
    assert not page.records
    batch, = page.batches
    assert batch.complete and batch.executionList and batch.invocationCount == 0
    page = invocation_page(client, run_id, "work")
    assert not page.batches
    assert [row.state for row in page.records] == [CardState.skipped]
    assert collected(client, tmp_path / "decode", run_id) == []
    journal = client.call("LogRangeRequest", runId=run_id, stream=LogStream.journal)
    assert any(diagnostic in line and "left activation 1" in line
               for line in text(journal.text).splitlines())


def test_singleton_run_list_and_vector_value_have_different_execution_shapes(core_factory, project, tmp_path):
    """A vector remains one value, while a singleton run list stays a one-item execution batch."""
    from harness.native_card import STATIC_RESULT_ENTRY, native_card

    client = core_factory(mocked_builds=True).connect()
    graph = Project(project)
    vector = 'table Vector { items:[ulong]; } table Outputs { out:Vector (required); } root_type Outputs;'
    vector_run = native_card(tmp_path / "vector", STATIC_RESULT_ENTRY, [
        ("out", encode_message(tmp_path / "encode", vector,
                                                    {"items": [1, 2, 3]}, root_type="Vector")),
    ])
    work_run = native_card(tmp_path / "work", STATIC_RESULT_ENTRY, [
        ("out", encode_message(tmp_path / "encode", NUMBER,
                                                    {"value": 42}, root_type="Number")),
    ])
    graph.card("vector", {"type": "code", "sandbox": {"filesystem": False}}, outputs=vector,
               plan=mock_plan([vector_run]))
    graph.card("work", {"type": "code", "sandbox": {"filesystem": False}, "recordIO": "on",
                        "inputs": [{"name": "in"}]}, outputs=NUMBER,
               plan=mock_plan([work_run], execution_list=True, labels=["forty-two"]))
    graph.connect("vector", "out", "work", "in")
    connect_collect(graph)
    graph.save(client)

    run_id = run(client, "vector", propagate=True)
    assert wait_run(client, run_id).state == RunState.succeeded
    vector_page = invocation_page(client, run_id, "vector")
    assert len(vector_page.records) == 1
    assert all(not batch.executionList for batch in vector_page.batches or [])
    work = invocation_page(client, run_id, "work")
    assert len(work.records) == 1
    assert text(work.records[0].implementationLabel) == "forty-two"
    batch, = work.batches
    assert batch.executionList and batch.invocationCount == 1
    assert recorded_input_json(client, tmp_path / "decode", run_id, "work") == {"items": [1, 2, 3]}
    assert collected(client, tmp_path / "decode", run_id) == [{"value": 42}]


def test_cartesian_limit_prevents_workers_and_partial_output(core_factory, project):
    """An oversized Cartesian product fails before the affected Card can run any combination."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    graph = Project(project).card("start", {"type": "trigger"})
    producer(graph, "left", range(17))
    producer(graph, "right", range(16))
    consumer(graph)
    connect_collect(graph)
    graph.save(client)

    run_id = run(client, "start", propagate=True)
    status = wait_run(client, run_id)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.execution_limit
    latest = client.call("RunLatestInvocationsRequest", runId=run_id)
    assert not any([text(segment) for segment in row.card.segments] == ["work"] and row.executionService
                   for row in latest.records)


@pytest.mark.parametrize("prepared", [False, True])
def test_implementation_limit_applies_before_running_batch(core_factory, project, tmp_path, prepared):
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    graph = Project(project).card("start", {"type": "trigger"})
    producer(graph, "left", range(17))
    producer(graph, "right", [1])
    consumer(graph, implementations=numeric_implementations(tmp_path, [99]) * 16)
    graph.save(client)

    if prepared:
        cards = tmp_path / "cards.bin"
        daemon.stop()
        daemon.args = ["--prepare-cards", str(cards)]
        daemon.start(expect_exit=True)
        assert daemon.proc.returncode == 0, daemon.diagnostics()
        daemon.args = ["--prepared-cards", str(cards)]
        daemon.start()
        client = daemon.connect()

    run_id = run(client, "start", propagate=True)
    status = wait_run(client, run_id)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.execution_limit
    row, = invocation_page(client, run_id, "work").records
    assert row.failure.kind == FailureKind.execution_limit and not row.executionService


def test_output_batch_limit_withholds_successful_members(core_factory, project, tmp_path):
    """Each output fits in a worker frame, but the aggregate exceeds the batch limit."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    body, = scripted_implementations(tmp_path, [
        [{"op": "output", "port": "out", "type": "string", "value": "x" * (9 * 1024 * 1024)}],
    ])
    schema = 'table Value { value:string; } table Outputs { out:Value; } root_type Outputs;'
    graph = Project(project).card("work", {"type": "code", "sandbox": {"filesystem": False}}, outputs=schema,
                                  plan=mock_plan([body] * 4, execution_list=True))
    connect_collect(graph)
    graph.save(client)

    run_id = run(client, propagate=True, maxConcurrency=1)
    status = wait_run(client, run_id)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.execution_limit
    batch, = invocation_page(client, run_id, "work").batches
    assert batch.complete and batch.failed and not batch.outputs


def test_collected_artifact_members_preserve_batch_order_and_grants(core_factory, project, tmp_path):
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    graph = Project(project)
    packages = scripted_implementations(tmp_path / "packages", [
        [{"op": "file", "port": "out", "path": "first.txt", "text": "one"}],
        [{"op": "directory", "port": "out", "path": "second", "files": {"data.txt": "two"}}],
    ])
    graph.card("work", {"type": "code", "recordIO": "on", "sandbox": {"filesystem": False}},
               outputs=ARTIFACT_OUTPUT, plan=mock_plan(packages, execution_list=True))
    connect_collect(graph)
    graph.card("consumer", {"type": "code", "recordIO": "on", "sandbox": {"filesystem": False},
                            "inputs": [{"name": "in"}]},
               outputs=NUMBER, plan=mock_plan(numeric_implementations(tmp_path / "consumer", [1])))
    graph.connect("collect", "out", "consumer", "in").save(client)

    run_id = run(client, "work", propagate=True)
    assert wait_run(client, run_id).state == RunState.succeeded

    def record(current, card, invocation=1):
        return current.call("RunRecordRequest", runId=run_id, card=path(card), invocation=invocation).record

    def check(current):
        members = [text(record(current, "work", index).outputs[0].artifacts[0].id)
                   for index in (1, 2)]
        assert len(set(members)) == 2
        value, = record(current, "collect").outputs
        envelopes = reference_json(tmp_path / "decode", bytes(value.schemaBfbs), bytes(value.inlineBytes),
                                   root_type="tsugumi.builtin.Collected")["items"]
        def label(item):
            key = bytes(item["artifacts"][0]["bytes"])
            return f"{int.from_bytes(key[:8], 'big')}.{int.from_bytes(key[8:], 'big')}"

        assert [label(item) for item in envelopes] == members
        received, = record(current, "consumer").inputs
        assert {text(item.id) for item in value.artifacts} == set(members)
        assert {text(item.id) for item in received.artifacts} == set(members)

    check(client)
    daemon.restart()
    check(daemon.connect())


def test_failed_batch_keeps_individual_history_and_cancels_its_sibling(core_factory, project, tmp_path):
    """A late failure keeps earlier success records, cancels a running sibling and withholds the batch."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    failure = Gate(tmp_path, "fail")
    hanging = Gate(tmp_path, "hanging")
    scenarios = [
        [{"op": "output", "port": "out", "type": "uint64", "value": 42}],
        [failure.action(), {"op": "failure", "message": "the batch member failed"}],
        [{"op": "hang", "entered": str(hanging.entered)}],
    ]
    graph = Project(project).card("work", {"type": "code", "sandbox": {"filesystem": False}, "recordIO": "on"},
                                  outputs=NUMBER, plan=mock_plan(scripted_implementations(tmp_path, scenarios), execution_list=True))
    connect_collect(graph)
    graph.save(client)

    run_id = run(client, "work", propagate=True, maxConcurrency=3)
    failure.wait()
    hanging.wait()
    wait_invocation(client, run_id, "work", lambda row: row.state == CardState.succeeded)
    failure.release()

    assert wait_run(client, run_id).state == RunState.failed
    page = invocation_page(client, run_id, "work")
    assert [row.state for row in page.records] == [CardState.succeeded, CardState.failed, CardState.cancelled]
    batch, = page.batches
    assert batch.complete and batch.failed and not batch.outputs
    latest = client.call("RunLatestInvocationsRequest", runId=run_id)
    assert not any([text(segment) for segment in row.card.segments] == ["collect"] and
                   row.state == CardState.succeeded for row in latest.records)


def test_interrupted_batch_recovers_each_running_members_position(core_factory, project, tmp_path):
    """After a daemon crash, individual records retain their position in the interrupted batch."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    gates = [Gate(tmp_path, "first"), Gate(tmp_path, "second")]
    implementations = scripted_implementations(tmp_path, [[gate.action()] for gate in gates])
    Project(project).card("work", {"type": "code", "sandbox": {"filesystem": False}}, outputs=NUMBER,
                          plan=mock_plan(implementations, execution_list=True)).save(client)
    run_id = run(client, maxConcurrency=2)
    try:
        for gate in gates:
            gate.wait()
        daemon.restart(crash=True)
    finally:
        for gate in gates:
            gate.release()
    client = daemon.connect()
    assert wait_run(client, run_id).state == RunState.failed
    page = invocation_page(client, run_id, "work")
    assert [(row.batchActivation, row.batchIndex, row.batchSize) for row in page.records] == [(1, 1, 2), (1, 2, 2)]
    assert all(row.state == CardState.failed and row.failure.kind == FailureKind.interrupted for row in page.records)
    batch, = page.batches
    assert batch.complete and batch.failed and not batch.outputs


def test_collect_keeps_input_order_when_members_finish_in_reverse(core_factory, project, tmp_path):
    """The consumer sees list order even when workers finish in the opposite order."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    gates = [Gate(tmp_path, str(index)) for index in range(3)]
    scenarios = [[gate.action(), {"op": "output", "port": "out", "type": "uint64", "value": index}]
                 for index, gate in enumerate(gates)]
    graph = Project(project).card("work", {"type": "code", "sandbox": {"filesystem": False}}, outputs=NUMBER,
                                  plan=mock_plan(scripted_implementations(tmp_path, scenarios), execution_list=True))
    connect_collect(graph)
    graph.save(client)

    run_id = run(client, propagate=True, maxConcurrency=3)
    for gate in gates:
        gate.wait()
    for index in reversed(range(len(gates))):
        gates[index].release()
        wait_invocation(client, run_id, "work",
                        lambda row: row.batchIndex == index + 1 and row.state == CardState.succeeded)
    assert wait_run(client, run_id).state == RunState.succeeded
    assert collected(client, tmp_path / "decode", run_id) == [{"value": 0}, {"value": 1}, {"value": 2}]


@pytest.mark.parametrize("limit,slots", [(64, 8), (2, 2)])
def test_eight_worker_cap_and_user_cancel_leave_no_partial_batch(core_factory, project, tmp_path, limit, slots):
    """User concurrency and the hard eight-worker cap hold until explicit cancellation."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    gates = [Gate(tmp_path, str(index)) for index in range(slots + 1)]
    scenarios = [[{"op": "hang", "entered": str(gate.entered)}] for gate in gates]
    graph = Project(project).card("work", {"type": "code", "sandbox": {"filesystem": False}}, outputs=NUMBER,
                                  plan=mock_plan(scripted_implementations(tmp_path, scenarios), execution_list=True))
    connect_collect(graph)
    graph.save(client)

    run_id = run(client, propagate=True, maxConcurrency=limit)
    for gate in gates[:slots]:
        gate.wait()
    assert not gates[slots].entered.exists()
    client.call("RunCancelRequest", runId=run_id)
    assert wait_run(client, run_id).state == RunState.cancelled
    assert not gates[slots].entered.exists()
    page = invocation_page(client, run_id, "work")
    assert [row.state for row in page.records] == [CardState.cancelled] * slots
    batch, = page.batches
    assert batch.invocationCount == slots + 1 and batch.complete and batch.failed and not batch.outputs
    latest = client.call("RunLatestInvocationsRequest", runId=run_id)
    assert not any(row.card.segments == [b"collect"] and row.state == CardState.succeeded
                   for row in latest.records)


def test_code_waits_for_connected_inputs_and_omits_unconnected_inputs(core_factory, project, tmp_path):
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    graph = Project(project).card("start", {"type": "trigger"})
    producer(graph, "left", [10, 20])
    producer(graph, "right", [30, 40, 50])
    graph.card("work", {"type": "code", "sandbox": {"filesystem": False}, "recordIO": "on",
                        "inputs": [{"name": name} for name in ("left", "right", "unconnected")]},
               outputs=NUMBER, plan=mock_plan(numeric_implementations(tmp_path, [1])))
    graph.connect("left", "out", "work", "left").connect("right", "out", "work", "right").save(client)

    run_id = run(client, "start", propagate=True)
    assert wait_run(client, run_id).state == RunState.succeeded
    rows = invocation_page(client, run_id, "work").records
    assert len(rows) == 6
    assert {row.batchActivation for row in rows} == {1}
    for row in rows:
        record = client.call("RunRecordRequest", runId=run_id, card=path("work"), invocation=row.invocation).record
        assert {text(value.name) for value in record.inputs} == {"left", "right"}

    rerun = run(client, "work", propagate=False)
    assert wait_run(client, rerun).state == RunState.succeeded
    assert len(invocation_page(client, rerun, "work").records) == 6


def test_merge_feedback_counts_batch_activations_toward_max_runs(core_factory, project, tmp_path):
    """A two-item batch takes two passes around a feedback loop before maxRuns stops it."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    graph = Project(project).card("start", {"type": "trigger"})
    producer(graph, "seed", [10, 20])
    graph.card("merge", {"type": "merge", "maxRuns": 2, "inputs": [{"name": "seed"}, {"name": "feedback"}]})
    graph.card("work", {"type": "code", "sandbox": {"filesystem": False}, "maxRuns": 3, "inputs": [{"name": "in"}]},
               outputs=NUMBER, plan=mock_plan(numeric_implementations(tmp_path, [1])))
    graph.connect("seed", "out", "merge", "seed").connect("merge", "out", "work", "in")
    graph.connect("work", "out", "merge", "feedback").save(client)

    run_id = run(client, "start", propagate=True)
    status = wait_run(client, run_id)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.execution_limit
    work = invocation_page(client, run_id, "work")
    assert [(row.batchActivation, row.batchIndex, row.batchSize) for row in work.records] == [
        (1, 1, 2), (1, 2, 2), (2, 1, 2), (2, 2, 2)]
    failed, = invocation_page(client, run_id, "merge").batches
    assert failed.activation == 3 and failed.complete and failed.failed and not failed.outputs


def test_per_run_worker_budget_stops_before_starting_an_over_budget_batch(core_factory, project, tmp_path):
    """The 4,096-worker limit covers all activations, including the source of a loop."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    body, = scripted_implementations(tmp_path, [[{"op": "output", "port": "out", "type": "uint64", "value": 1}]])
    graph = Project(project).card("start", {"type": "trigger"})
    graph.card("seed", {"type": "code", "sandbox": {"filesystem": False}}, outputs=NUMBER,
               plan=mock_plan([body] * 256, execution_list=True))
    graph.card("merge", {"type": "merge", "maxRuns": 20, "inputs": [{"name": "seed"}, {"name": "feedback"}]})
    graph.card("work", {"type": "code", "sandbox": {"filesystem": False}, "reuseWorker": True, "maxRuns": 20,
                        "inputs": [{"name": "in"}]}, outputs=NUMBER, plan=mock_plan([body]))
    graph.connect("seed", "out", "merge", "seed").connect("merge", "out", "work", "in")
    graph.connect("work", "out", "merge", "feedback").save(client)

    run_id = run(client, "start", propagate=True, maxConcurrency=8)
    status = wait_run(client, run_id)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.execution_limit
    seed, = invocation_page(client, run_id, "seed").batches
    work, = invocation_page(client, run_id, "work").batches
    assert seed.complete and not seed.failed and seed.invocationCount == 256
    assert work.complete and work.failed and not work.outputs and work.invocationCount == 256
    assert work.activation == 16  # seed and fifteen completed work batches used the whole budget
    latest = client.call("RunLatestInvocationsRequest", runId=run_id)
    rejected = next(row for row in latest.records if row.card.segments == [b"work"])
    assert rejected.batchActivation == 16 and rejected.state == CardState.failed and not rejected.executionService
