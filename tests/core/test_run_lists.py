"""Exercise Cartesian batches, collection and history through the public RPC."""

from itertools import product
import json
import shutil
import time

from harness.observations import read_output
from harness.scenario import Gate, Project, run, wait_run
from harness.values import recorded_input_json, reference_json
from tsugumi_client import path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.RPC.RunEventValue import RunEventValue


NUMBER = 'table Number { value:ulong; } table Outputs { out:Number (required); } root_type Outputs;'
DIRECTORY = ('attribute "tsugumi"; table Directory { value:string; } '
             'table Outputs { out:Directory (tsugumi:"directory"); } root_type Outputs;')


def implementation(value):
    output = json.dumps({"out": {"value": value}})
    return {"bash": f"printf '%s' '{output}' > \"$TSUGUMI_OUTPUT\""}


def scripted_implementations(directory, scenarios):
    implementations = []
    for index, actions in enumerate(scenarios):
        package = directory / f"package-{index}"
        package.mkdir()
        (package / "scenario.json").write_text(json.dumps({"actions": actions}))
        implementations.append({"package": str(package)})
    return implementations


def producer(graph, name, values):
    graph.card(name, {"language": "bash", "sandbox": False}, outputs=NUMBER,
               manifest={"runs": [implementation(value) for value in values]})


def consumer(graph, *, implementations=None):
    manifest = {"run": implementation(99)} if implementations is None else {"runs": implementations}
    graph.card("work", {"language": "bash", "sandbox": False, "recordIO": True, "maxRuns": 1,
                        "inputs": [{"name": "left"}, {"name": "right"}]},
               outputs=NUMBER, manifest=manifest)
    graph.connect("left", "out", "work", "left").connect("right", "out", "work", "right")


def connect_collect(graph):
    graph.card("collect", {"template": "collect", "recordIO": True})
    graph.connect("work", "out", "collect", "in")


def invocation_page(client, run_id, card):
    return client.call("RunInvocationsRequest", runId=run_id, card=path(card))


def collected(client, directory, run_id, card="collect"):
    record = client.call("RunRecordRequest", runId=run_id, card=path(card)).record
    value, = record.outputs
    payload = read_output(client, run_id, card=card)
    return reference_json(directory, bytes(value.schemaBfbs), payload,
                          root_type=text(value.rootType))["items"]


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
    daemon = core_factory(mocked_builds=True, history_dir=history_dir)
    client = daemon.connect()
    graph = Project(project).card("start", {"template": "trigger"})
    producer(graph, "left", [10, 20])
    producer(graph, "right", [1, 2, 3])
    consumer(graph, implementations=[implementation(101), implementation(202)])
    connect_collect(graph)
    graph.save(client)
    opened = client.open()
    client.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId), cards=[path("work")])

    run_id = run(client, "start", propagate=True)
    assert wait_run(client, run_id, timeout=60).state == RunState.succeeded

    def work_batch(note):
        if note.valueType != NotificationPayload.RunEvent:
            return False
        event = note.value
        return (text(event.runId) == run_id and event.valueType == RunEventValue.RunBatchChanged
                and event.value.batch.card.segments == [b"work"])

    started = client.wait_notification(work_batch).value.value.batch
    finished = client.wait_notification(work_batch).value.value.batch
    assert started.activation == finished.activation == 1
    assert started.invocationCount == finished.invocationCount == 12
    assert not started.complete and finished.complete and not finished.failed

    expected = list(product([10, 20], [1, 2, 3], [1, 2]))
    page, actual = combinations(client, tmp_path / "decode", run_id)
    assert actual == expected
    assert [row.batchIndex for row in page.records] == list(range(1, 13))
    assert all(row.batchActivation == 1 and row.batchSize == 12 for row in page.records)
    batch, = page.batches
    assert batch.complete and not batch.failed and batch.executionList and batch.invocationCount == 12
    assert collected(client, tmp_path / "decode", run_id) == [{"value": 101}, {"value": 202}] * 6

    daemon.restart()
    client = daemon.connect()
    saved, = invocation_page(client, run_id, "work").batches
    assert saved.complete and saved.invocationCount == 12
    replay = run(client, "work", propagate=False)
    assert wait_run(client, replay, timeout=60).state == RunState.succeeded
    assert combinations(client, tmp_path / "decode", replay)[1] == expected

    daemon.stop()
    copied = tmp_path / "copied-project"
    shutil.copytree(project, copied, symlinks=True)
    restored = core_factory(project_root=copied, history_dir=tmp_path / "restored-history").connect()
    batch, = invocation_page(restored, run_id, "work").batches
    assert batch.complete and batch.invocationCount == 12
    assert combinations(restored, tmp_path / "decode", run_id)[1] == expected


def test_empty_batch_is_delivered_to_collect_without_starting_a_worker(core_factory, project, tmp_path):
    """An empty run list produces a typed empty vector through a downstream scalar implementation."""
    client = core_factory(mocked_builds=True).connect()
    graph = Project(project).card("start", {"template": "trigger"})
    producer(graph, "left", [])
    producer(graph, "right", [1])
    consumer(graph)
    connect_collect(graph)
    graph.save(client)

    run_id = run(client, "start", propagate=True)
    assert wait_run(client, run_id).state == RunState.succeeded
    for name in ("left", "work"):
        page = invocation_page(client, run_id, name)
        assert not page.records
        batch, = page.batches
        assert batch.complete and batch.executionList and batch.invocationCount == 0
    assert collected(client, tmp_path / "decode", run_id) == []

    replay = run(client, "work", propagate=False)
    assert wait_run(client, replay).state == RunState.succeeded
    page = invocation_page(client, replay, "work")
    assert not page.records
    batch, = page.batches
    assert batch.complete and batch.executionList and batch.invocationCount == 0


def test_singleton_run_list_and_vector_value_have_different_execution_shapes(core_factory, project, tmp_path):
    """A vector remains one value, while a singleton run list stays a one-item execution batch."""
    client = core_factory(mocked_builds=True).connect()
    graph = Project(project).card("start", {"template": "trigger"})
    vector = 'table Vector { items:[ulong]; } table Outputs { out:Vector (required); } root_type Outputs;'
    graph.card("vector", {"language": "bash", "sandbox": False}, outputs=vector,
               manifest={"run": {"bash": "printf '%s' '{\"out\":{\"items\":[1,2,3]}}' > \"$TSUGUMI_OUTPUT\""}})
    graph.card("work", {"language": "bash", "sandbox": False, "recordIO": True,
                        "inputs": [{"name": "in"}]}, outputs=NUMBER,
               manifest={"runs": [implementation(42)]})
    graph.connect("vector", "out", "work", "in")
    connect_collect(graph)
    graph.save(client)

    run_id = run(client, "start", propagate=True)
    assert wait_run(client, run_id).state == RunState.succeeded
    vector_page = invocation_page(client, run_id, "vector")
    assert len(vector_page.records) == 1
    assert all(not batch.executionList for batch in vector_page.batches or [])
    work = invocation_page(client, run_id, "work")
    assert len(work.records) == 1
    batch, = work.batches
    assert batch.executionList and batch.invocationCount == 1
    assert recorded_input_json(client, tmp_path / "decode", run_id, "work") == {"items": [1, 2, 3]}
    assert collected(client, tmp_path / "decode", run_id) == [{"value": 42}]


def test_branch_filters_cartesian_batch_and_sends_an_empty_list(core_factory, project, tmp_path):
    """Fork preserves the list, and both Branch outputs keep its shape."""
    client = core_factory(mocked_builds=True).connect()
    graph = Project(project).card("start", {"template": "trigger"})
    producer(graph, "values", [10, 20])
    flag = 'table Flag { value:bool; } table Outputs { out:Flag (required); } root_type Outputs;'
    yes = {"bash": "printf '%s' '{\"out\":{\"value\":true}}' > \"$TSUGUMI_OUTPUT\""}
    graph.card("flags", {"language": "bash", "sandbox": False}, outputs=flag,
               manifest={"runs": [yes, yes]})
    graph.card("fork", {"template": "fork"})
    graph.card("branch", {"template": "branch"})
    graph.card("selected", {"template": "collect", "recordIO": True})
    graph.card("empty", {"template": "collect", "recordIO": True})
    graph.connect("values", "out", "fork", "in").connect("fork", "out1", "branch", "value")
    graph.connect("flags", "out", "branch", "condition")
    graph.connect("branch", "a", "selected", "in").connect("branch", "b", "empty", "in")
    graph.save(client)

    run_id = run(client, "start", propagate=True)
    assert wait_run(client, run_id).state == RunState.succeeded
    batch, = invocation_page(client, run_id, "branch").batches
    assert batch.executionList and batch.invocationCount == 4
    assert {text(output.port): (output.count, output.executionList)
            for output in batch.outputs} == {"a": (4, True), "b": (0, True)}
    selected = collected(client, tmp_path / "decode", run_id, "selected")
    assert sorted(item["value"] for item in selected) == [10, 10, 20, 20]
    assert collected(client, tmp_path / "decode", run_id, "empty") == []


def test_cartesian_limit_prevents_workers_and_partial_output(core_factory, project):
    """An oversized Cartesian product fails before the affected Card can run any combination."""
    client = core_factory(mocked_builds=True).connect()
    graph = Project(project).card("start", {"template": "trigger"})
    producer(graph, "left", range(17))
    producer(graph, "right", range(16))
    consumer(graph)
    connect_collect(graph)
    graph.save(client)

    run_id = run(client, "start", propagate=True)
    status = wait_run(client, run_id, timeout=60)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.execution_limit
    latest = client.call("RunLatestInvocationsRequest", runId=run_id)
    assert not any([text(segment) for segment in row.card.segments] == ["work"] and row.executionService
                   for row in latest.records)


def test_output_batch_limit_withholds_successful_members(core_factory, project):
    """Each output fits in a worker frame, but the aggregate exceeds the batch limit."""
    client = core_factory(mocked_builds=True).connect()
    body = r'''
        { printf '%s' '{"out":{"value":"'; head -c 9437184 /dev/zero | tr '\000' x; printf '%s' '"}}'; } > "$TSUGUMI_OUTPUT"
    '''
    schema = 'table Value { value:string; } table Outputs { out:Value; } root_type Outputs;'
    graph = Project(project).card("work", {"language": "bash", "sandbox": False}, outputs=schema,
                                  manifest={"runs": [{"bash": body}] * 4})
    connect_collect(graph)
    graph.save(client)

    run_id = run(client, propagate=True, maxConcurrency=1)
    status = wait_run(client, run_id, timeout=60)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.execution_limit
    batch, = invocation_page(client, run_id, "work").batches
    assert batch.complete and batch.failed and not batch.outputs


def test_collected_directories_and_saved_batch_keep_empty_subdirectories(core_factory, project):
    """Directory batches retain each item's tree during collection and a saved-input rerun."""
    daemon = core_factory(mocked_builds=True)
    client = daemon.connect()
    graph = Project(project).card("start", {"template": "trigger"})
    emit = '''jq -nc --arg dir "$1" '{out:{value:$dir}}' > "$TSUGUMI_OUTPUT"'''
    runs = [{"bash": f'''
        dir="$TSUGUMI_OUTPUT_DIR/sample"
        mkdir -p "$dir/empty/inner"
        printf '%s\\n' {number} > "$dir/data.txt"
        set -- "$dir"
        {emit}
    '''} for number in (1, 2)]
    graph.card("source", {"language": "bash", "sandbox": False}, outputs=DIRECTORY, manifest={"runs": runs})
    graph.card("work", {"language": "bash", "sandbox": False, "inputs": [{"name": "in"}]},
               outputs=DIRECTORY, manifest={"run": {"bash": f'''
                   test -d "$DIR_IN/empty/inner"
                   cat "$DIR_IN/data.txt"
                   set -- "$DIR_IN"
                   {emit}
               '''}})
    graph.connect("source", "out", "work", "in")
    connect_collect(graph)
    graph.card("check", {"language": "bash", "sandbox": False, "inputs": [{"name": "in"}]},
               outputs=NUMBER, manifest={"run": {"bash": '''
                   test "$(find "$DIR_IN" -type d -name inner | wc -l | tr -d ' ')" = 2
                   test "$(find "$DIR_IN" -type f -name data.txt | wc -l | tr -d ' ')" = 2
                   printf '%s' '{"out":{"value":2}}' > "$TSUGUMI_OUTPUT"
               '''}})
    graph.connect("collect", "out", "check", "in")
    graph.save(client)

    run_id = run(client, "start", propagate=True)
    assert wait_run(client, run_id, timeout=60).state == RunState.succeeded
    assert len(invocation_page(client, run_id, "check").records) == 1
    daemon.restart()
    client = daemon.connect()
    replay = run(client, "work", propagate=True)
    assert wait_run(client, replay, timeout=60).state == RunState.succeeded
    page = invocation_page(client, replay, "work")
    assert len(page.records) == 2
    logs = [client.call("LogRangeRequest", runId=replay, card=path("work"), invocation=row.invocation,
                        stream=LogStream.stdout) for row in page.records]
    assert [text(log.text) for log in logs] == ["1\n", "2\n"]


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
    graph = Project(project).card("work", {"language": "bash", "sandbox": False, "recordIO": True},
                                  outputs=NUMBER, manifest={"runs": scripted_implementations(tmp_path, scenarios)})
    connect_collect(graph)
    graph.save(client)

    run_id = run(client, "work", propagate=True, maxConcurrency=3)
    failure.wait()
    hanging.wait()
    deadline = time.monotonic() + 5
    while not any(row.state == CardState.succeeded for row in invocation_page(client, run_id, "work").records):
        assert time.monotonic() < deadline, "the first member did not complete"
        time.sleep(0.01)
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
    Project(project).card("work", {"language": "bash", "sandbox": False}, outputs=NUMBER,
                          manifest={"runs": implementations}).save(client)
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
    graph = Project(project).card("work", {"language": "bash", "sandbox": False}, outputs=NUMBER,
                                  manifest={"runs": scripted_implementations(tmp_path, scenarios)})
    connect_collect(graph)
    graph.save(client)

    run_id = run(client, propagate=True, maxConcurrency=3)
    for gate in gates:
        gate.wait()
    for index in reversed(range(len(gates))):
        gates[index].release()
        deadline = time.monotonic() + 5
        while invocation_page(client, run_id, "work").records[index].state != CardState.succeeded:
            assert time.monotonic() < deadline, f"member {index + 1} did not finish"
            time.sleep(0.01)
    assert wait_run(client, run_id).state == RunState.succeeded
    assert collected(client, tmp_path / "decode", run_id) == [{"value": 0}, {"value": 1}, {"value": 2}]


def test_eight_worker_cap_and_user_cancel_leave_no_partial_batch(core_factory, project, tmp_path):
    """A Run can cancel the workers occupying all eight slots without publishing a partial batch."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    gates = [Gate(tmp_path, str(index)) for index in range(9)]
    scenarios = [[{"op": "hang", "entered": str(gate.entered)}] for gate in gates]
    graph = Project(project).card("work", {"language": "bash", "sandbox": False}, outputs=NUMBER,
                                  manifest={"runs": scripted_implementations(tmp_path, scenarios)})
    connect_collect(graph)
    graph.save(client)

    run_id = run(client, propagate=True, maxConcurrency=64)
    for gate in gates[:8]:
        gate.wait(timeout=10)
    assert not gates[8].entered.exists()
    client.call("RunCancelRequest", runId=run_id)
    assert wait_run(client, run_id, timeout=20).state == RunState.cancelled
    assert not gates[8].entered.exists()
    page = invocation_page(client, run_id, "work")
    assert [row.state for row in page.records] == [CardState.cancelled] * 8
    batch, = page.batches
    assert batch.invocationCount == 9 and batch.complete and batch.failed and not batch.outputs
    latest = client.call("RunLatestInvocationsRequest", runId=run_id)
    assert not any(row.card.segments == [b"collect"] and row.state == CardState.succeeded
                   for row in latest.records)


def test_each_input_code_consumes_one_complete_batch_per_arrival(core_factory, project, tmp_path):
    client = core_factory(mocked_builds=True).connect()
    graph = Project(project).card("start", {"template": "trigger"})
    producer(graph, "left", [10, 20])
    producer(graph, "right", [30, 40, 50])
    graph.card("work", {"language": "bash", "sandbox": False, "recordIO": True,
                        "trigger": "eachInput", "maxRuns": 2,
                        "inputs": [{"name": "left", "optional": True}, {"name": "right", "optional": True}]},
               outputs=NUMBER, manifest={"run": implementation(1)})
    graph.connect("left", "out", "work", "left").connect("right", "out", "work", "right").save(client)

    run_id = run(client, "start", propagate=True)
    assert wait_run(client, run_id, timeout=20).state == RunState.succeeded
    rows = invocation_page(client, run_id, "work").records
    assert len(rows) == 5
    assert {row.batchActivation for row in rows} == {1, 2}
    arrivals = {}
    for row in rows:
        record = client.call("RunRecordRequest", runId=run_id, card=path("work"), invocation=row.invocation).record
        value, = record.inputs
        port = text(value.name)
        item = recorded_input_json(client, tmp_path / "decode", run_id, "work", invocation=row.invocation, port=port)
        arrivals.setdefault(row.batchActivation, []).append((port, item["value"]))
    assert sorted(arrivals.values()) == [[("left", 10), ("left", 20)], [("right", 30), ("right", 40), ("right", 50)]]
    for activation, members in arrivals.items():
        assert [row.batchIndex for row in rows if row.batchActivation == activation] == list(range(1, len(members) + 1))


def test_merge_feedback_counts_batch_activations_toward_max_runs(core_factory, project):
    """A two-item batch takes two passes around a feedback loop before maxRuns stops it."""
    client = core_factory(mocked_builds=True).connect()
    graph = Project(project).card("start", {"template": "trigger"})
    producer(graph, "seed", [10, 20])
    graph.card("merge", {"template": "merge", "maxRuns": 2, "inputs": [{"name": "seed"}, {"name": "feedback"}]})
    graph.card("work", {"language": "bash", "sandbox": False, "maxRuns": 3, "inputs": [{"name": "in"}]},
               outputs=NUMBER, manifest={"run": implementation(1)})
    graph.connect("seed", "out", "merge", "seed").connect("merge", "out", "work", "in")
    graph.connect("work", "out", "merge", "feedback").save(client)

    run_id = run(client, "start", propagate=True)
    status = wait_run(client, run_id, timeout=20)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.execution_limit
    assert "maxRuns 2" in text(status.failure.diagnostic)
    work = invocation_page(client, run_id, "work")
    assert [(row.batchActivation, row.batchIndex, row.batchSize) for row in work.records] == [
        (1, 1, 2), (1, 2, 2), (2, 1, 2), (2, 2, 2)]
    failed, = invocation_page(client, run_id, "merge").batches
    assert failed.activation == 3 and failed.complete and failed.failed and not failed.outputs


def test_per_run_worker_budget_stops_before_starting_an_over_budget_batch(core_factory, project, tmp_path):
    """The 4,096-worker limit covers all activations, including the source of a loop."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    body, = scripted_implementations(tmp_path, [[{"op": "output", "port": "out", "type": "uint64", "value": 1}]])
    graph = Project(project).card("start", {"template": "trigger"})
    graph.card("seed", {"language": "bash", "sandbox": False}, outputs=NUMBER,
               manifest={"runs": [body] * 256})
    graph.card("merge", {"template": "merge", "maxRuns": 20, "inputs": [{"name": "seed"}, {"name": "feedback"}]})
    graph.card("work", {"language": "bash", "sandbox": False, "reuseWorker": True, "maxRuns": 20,
                        "inputs": [{"name": "in"}]}, outputs=NUMBER, manifest={"run": body})
    graph.connect("seed", "out", "merge", "seed").connect("merge", "out", "work", "in")
    graph.connect("work", "out", "merge", "feedback").save(client)

    run_id = run(client, "start", propagate=True, maxConcurrency=8)
    status = wait_run(client, run_id, timeout=180)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.execution_limit
    assert "4096 worker invocations" in text(status.failure.diagnostic)
    seed, = invocation_page(client, run_id, "seed").batches
    work, = invocation_page(client, run_id, "work").batches
    assert seed.complete and not seed.failed and seed.invocationCount == 256
    assert work.complete and work.failed and not work.outputs and work.invocationCount == 256
    assert work.activation == 16  # seed and fifteen completed work batches used the whole budget
    latest = client.call("RunLatestInvocationsRequest", runId=run_id)
    rejected = next(row for row in latest.records if row.card.segments == [b"work"])
    assert rejected.batchActivation == 16 and rejected.state == CardState.failed and not rejected.executionService
