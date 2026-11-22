"""Exercise Cartesian batches, collection and history through the public RPC."""

from itertools import product
import json
import shutil

import pytest

from projects import wait_invocation
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


def mock_plan(runs, *, execution_list=False, **options):
    return {"product": {"runs": runs, "views": []}, "executionList": execution_list, **options}


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
    graph.card("work", {"type": "code", "sandbox": {"filesystem": False}, "recordIO": True, "maxRuns": 1,
                        "inputs": [{"name": "left"}, {"name": "right"}]},
               outputs=NUMBER, plan=mock_plan(runs, execution_list=implementations is not None))
    graph.connect("left", "out", "work", "left").connect("right", "out", "work", "right")


def connect_collect(graph):
    graph.card("collect", {"type": "collect", "recordIO": True})
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
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    graph = Project(project).card("start", {"type": "trigger"})
    producer(graph, "left", [10, 20])
    producer(graph, "right", [1, 2])
    consumer(graph, implementations=numeric_implementations(tmp_path, [101, 202]))
    connect_collect(graph)
    graph.save(client)
    opened = client.open()
    client.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId), cards=[path("work")])

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
    """An empty run list produces a typed empty vector through a downstream scalar implementation."""
    diagnostic = "the evaluation selected no implementations"
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    graph = Project(project).card("start", {"type": "trigger"})
    graph.card("left", {"type": "code", "sandbox": {"filesystem": False}}, outputs=NUMBER,
               plan=mock_plan([], execution_list=True, log=diagnostic))
    producer(graph, "right", [1])
    consumer(graph)
    connect_collect(graph)
    graph.save(client)

    opened = client.open()
    client.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId),
                cards=[path("left"), path("work")])

    run_id = run(client, "start", propagate=True)
    assert wait_run(client, run_id).state == RunState.succeeded
    for name in ("left", "work"):
        page = invocation_page(client, run_id, name)
        assert not page.records
        batch, = page.batches
        assert batch.complete and batch.executionList and batch.invocationCount == 0
    assert collected(client, tmp_path / "decode", run_id) == []
    journal = client.call("LogRangeRequest", runId=run_id, stream=LogStream.journal)
    lines = [json.loads(line) for line in text(journal.text).splitlines()]
    assert any(diagnostic in line["message"] and "left activation 1" in line["message"]
               for line in lines)

    assert not any(
        note.valueType == NotificationPayload.RunEvent
        and text(note.value.runId) == run_id
        and note.value.valueType == RunEventValue.RunCardChanged
        and note.value.value.invocation.card.segments in ([b"left"], [b"work"])
        for note in client.notifications
    )

    replay = run(client, "work", propagate=False)
    assert wait_run(client, replay).state == RunState.succeeded
    page = invocation_page(client, replay, "work")
    assert not page.records
    batch, = page.batches
    assert batch.complete and batch.executionList and batch.invocationCount == 0


def abi9_card(body, run="package", builder="inputs.tsugumi.cppCard { src = ./.; }"):
    return {
        "default.nix": "{ pkgs, inputs, params }: let package = " + builder
                       + "; in { run = " + run + "; }",
        "main.cc": ('#include "card.hh"\n#include <filesystem>\n#include <fstream>\n'
                    '#include <iostream>\n#include <stdexcept>\n' + body),
    }


def test_singleton_run_list_and_vector_value_have_different_execution_shapes(core_factory, project, tmp_path):
    """A vector remains one value, while a singleton run list stays a one-item execution batch."""
    client = core_factory().connect()
    graph = Project(project).card("start", {"type": "trigger"})
    vector = 'table Vector { items:[ulong]; } table Outputs { out:Vector (required); } root_type Outputs;'
    graph.card("vector", {"type": "code", "sandbox": {"filesystem": False}}, outputs=vector,
               sources=abi9_card("""
                 auto run(const Card::Inputs&) -> Card::Output {
                   Card::Output output;
                   output.out.emplace().items = {1, 2, 3};
                   return output;
                 }"""))
    graph.card("work", {"type": "code", "sandbox": {"filesystem": False}, "recordIO": True,
                        "inputs": [{"name": "in"}]}, outputs=NUMBER,
               sources=abi9_card("""
                 auto run(const Card::Inputs&) -> Card::Output {
                   Card::Output output;
                   output.out.emplace().value = 42;
                   return output;
                 }""", "[ { label = \"forty-two\"; run = package; } ]"))
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
    assert text(work.records[0].implementationLabel) == "forty-two"
    batch, = work.batches
    assert batch.executionList and batch.invocationCount == 1
    assert recorded_input_json(client, tmp_path / "decode", run_id, "work") == {"items": [1, 2, 3]}
    assert collected(client, tmp_path / "decode", run_id) == [{"value": 42}]


def test_branch_filters_cartesian_batch_and_sends_an_empty_list(core_factory, project, tmp_path):
    """Direct fan-out and both Branch outputs preserve list shape."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    graph = Project(project).card("start", {"type": "trigger"})
    producer(graph, "values", [10, 20])
    flag = 'table Flag { value:bool; } table Outputs { out:Flag (required); } root_type Outputs;'
    yes, = scripted_implementations(tmp_path, [
        [{"op": "output", "port": "out", "type": "bool", "value": True}],
    ])
    graph.card("flags", {"type": "code", "sandbox": {"filesystem": False}}, outputs=flag,
               plan=mock_plan([yes, yes], execution_list=True))
    graph.card("branch", {"type": "branch"})
    graph.card("selected", {"type": "collect", "recordIO": True})
    graph.card("empty", {"type": "collect", "recordIO": True})
    graph.card("original", {"type": "collect", "recordIO": True})
    graph.connect("values", "out", "branch", "value").connect("values", "out", "original", "in")
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
    assert sorted(item["value"] for item in collected(client, tmp_path / "decode", run_id, "original")) == [10, 20]


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
        cards = tmp_path / "cards.jsonl"
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


def test_collected_directories_and_saved_batch_keep_empty_subdirectories(core_factory, project):
    """Directory batches retain each item's tree during collection and a saved-input rerun."""
    daemon = core_factory()
    client = daemon.connect()
    graph = Project(project).card("start", {"type": "trigger"})
    source = abi9_card("""
      auto run(const Card::Inputs&) -> Card::Output {
        std::filesystem::create_directories("sample/empty/inner");
        std::ofstream("sample/data.txt") << VALUE << '\\n';
        Card::Output output;
        output.out.emplace().value = "sample";
        return output;
      }""", "[ (package 1) (package 2) ]",
        'number: inputs.tsugumi.cppCard { src = ./.; cxxflags = [ "-DVALUE=${toString number}" ]; }')
    graph.card("source", {"type": "code", "sandbox": {"filesystem": False}}, outputs=DIRECTORY,
               sources=source)
    graph.card("work", {"type": "code", "sandbox": {"filesystem": False}, "inputs": [{"name": "in"}]},
               outputs=DIRECTORY, sources=abi9_card("""
                 auto run(const Card::Inputs& input) -> Card::Output {
                   const std::filesystem::path folder(input.in.value);
                   if (!std::filesystem::is_directory(folder / "empty/inner"))
                     throw std::runtime_error("missing empty directory");
                   std::cout << std::ifstream(folder / "data.txt").rdbuf();
                   Card::Output output;
                   output.out.emplace().value = input.in.value;
                   return output;
                 }"""))
    graph.connect("source", "out", "work", "in")
    connect_collect(graph)
    graph.card("check", {"type": "code", "sandbox": {"filesystem": False}, "inputs": [{"name": "in"}]},
               outputs=NUMBER, sources=abi9_card("""
                 auto run(const Card::Inputs& input) -> Card::Output {
                   int files = 0;
                   int empty = 0;
                   for (const auto& entry : std::filesystem::recursive_directory_iterator(input.in.value)) {
                     if (entry.is_directory() && entry.path().filename() == "inner") ++empty;
                     if (entry.is_regular_file() && entry.path().filename() == "data.txt") ++files;
                   }
                   if (files != 2 || empty != 2) throw std::runtime_error("incomplete collection");
                   Card::Output output;
                   output.out.emplace().value = files;
                   return output;
                 }"""))
    graph.connect("collect", "out", "check", "in")
    graph.save(client)

    run_id = run(client, "start", propagate=True)
    assert wait_run(client, run_id).state == RunState.succeeded
    assert len(invocation_page(client, run_id, "check").records) == 1
    daemon.restart()
    client = daemon.connect()
    replay = run(client, "work", propagate=True)
    assert wait_run(client, replay).state == RunState.succeeded
    page = invocation_page(client, replay, "work")
    assert len(page.records) == 2
    logs = [client.call("LogRangeRequest", runId=replay, card=path("work"), invocation=row.invocation,
                        stream=LogStream.standard_output) for row in page.records]
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
    graph = Project(project).card("work", {"type": "code", "sandbox": {"filesystem": False}, "recordIO": True},
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
    graph.card("work", {"type": "code", "sandbox": {"filesystem": False}, "recordIO": True,
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
