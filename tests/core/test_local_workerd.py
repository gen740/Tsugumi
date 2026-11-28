"""Local slots belong to workerd, and end with their starting core."""

import json
import sys
import time

import pytest

from harness.daemon import WORKERD
from harness.fixtures import SCRIPTED_WORKER_SANDBOX, mock_plan
from harness.file_events import wait_for_exit
from harness.scenario import Gate, Project, run, wait_run
from tsugumi_client import RpcFailure, path, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState


def local_service(directory):
    """Observe our chosen executable without enumerating host processes.

    Worker PIDs are in a bwrap PID namespace on Linux; the service PID is
    always a host PID, so ownership assertions use this wrapper's identity.
    """
    marker = directory / "workerd.started"
    launcher = directory / "workerd"
    launcher.write_text(
        f"#!{sys.executable}\nimport os, json, sys\n"
        f"from pathlib import Path\nPath({str(marker)!r}).write_text("
        "json.dumps({'pid': os.getpid(), 'parent_pid': os.getppid()}))\n"
        f"os.execv({str(WORKERD)!r}, [{str(WORKERD)!r}, *sys.argv[1:]])\n"
    )
    launcher.chmod(0o700)
    return launcher, marker


@pytest.mark.parametrize("resident", [False, True])
def test_core_crash_ends_the_service_owning_an_active_worker(core_factory, project, tmp_path, resident):
    """A live local call's service ends when its starting core dies."""
    launcher, service_marker = local_service(tmp_path)
    daemon = core_factory(mocked_builds=True, mocked_worker=True, args=["--workerd", str(launcher)])
    client = daemon.connect()
    gate = Gate(tmp_path, "active")
    Project(project).card(
        "work", {"type": "code", "sandbox": SCRIPTED_WORKER_SANDBOX, "reuseWorker": resident},
        outputs="table Outputs {} root_type Outputs;", plan=mock_plan([str(project / "definitions/work")]),
        sources={"scenario.json": json.dumps({"actions": [gate.action()]})},
    ).save(client)
    run_id = run(client)
    gate.wait()
    owner = json.loads(service_marker.read_text())
    assert owner["parent_pid"] == daemon.proc.pid
    daemon.stop(crash=True)
    wait_for_exit(owner["pid"])
    daemon.start()
    status = daemon.connect().call("RunStatusRequest", runId=run_id)
    assert status.state == RunState.failed
    assert status.failure.kind == FailureKind.interrupted
    assert json.loads(service_marker.read_text()) == owner


def test_resident_connection_returns_with_its_call_counter(core_factory, project, tmp_path):
    """Lending a resident connection again preserves its identity and ordered call IDs."""
    launcher, service_marker = local_service(tmp_path)
    daemon = core_factory(mocked_builds=True, mocked_worker=True, args=["--workerd", str(launcher)])
    client = daemon.connect()
    gate = Gate(tmp_path, "resident")
    gate.release()
    Project(project).card(
        "work", {"type": "code", "sandbox": SCRIPTED_WORKER_SANDBOX, "reuseWorker": True},
        outputs="table Outputs {} root_type Outputs;", plan=mock_plan([str(project / "definitions/work")]),
        sources={"scenario.json": json.dumps({"actions": [gate.action()]})},
    ).save(client)
    observations = []
    for expected in (1, 2):
        run_id = run(client)
        assert wait_run(client, run_id).state == RunState.succeeded
        observations.append(json.loads(gate.entered.read_text()))
        record, = client.call("RunInvocationsRequest", runId=run_id, card=path("work")).records
        assert record.workerReused == (expected == 2)
        assert text(record.executionService) == "local"
    assert observations[0]["pid"] == observations[1]["pid"]
    assert observations[0]["parent_pid"] == observations[1]["parent_pid"]
    assert [value["call"] for value in observations] == [1, 2]
    owner = json.loads(service_marker.read_text())
    assert owner["parent_pid"] == daemon.proc.pid
    daemon.stop(crash=True)
    wait_for_exit(owner["pid"])
    daemon.start()
    client = daemon.connect()
    next_run = run(client)
    assert wait_run(client, next_run).state == RunState.succeeded
    record, = client.call("RunInvocationsRequest", runId=next_run, card=path("work")).records
    assert not record.workerReused


def test_running_card_rejects_definition_edit_and_retires_after_save(core_factory, project, tmp_path):
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    gate = Gate(tmp_path, "retired")
    definition = {"type": "code", "sandbox": SCRIPTED_WORKER_SANDBOX, "reuseWorker": True}

    def project_with(script):
        return Project(project).card(
            "work", definition, outputs="table Outputs {} root_type Outputs;",
            plan=mock_plan([str(project / "definitions/work")]),
            sources={"scenario.json": json.dumps({"actions": [gate.action()]}), "src/version.txt": script})

    project_with("old").save(client)
    current = run(client)
    gate.wait()
    try:
        with pytest.raises(RpcFailure) as error:
            project_with("new").save(client)
        assert error.value.kind == "busy"
    finally:
        gate.release()
    assert wait_run(client, current).state == RunState.succeeded
    project_with("new").save(client)
    replaced = run(client)
    assert wait_run(client, replaced).state == RunState.succeeded
    invocation, = client.call("RunInvocationsRequest", runId=replaced, card=path("work")).records
    assert not invocation.workerReused


def active_row(client, run_id, check, timeout=30):
    deadline = time.monotonic() + timeout
    rows = []
    while time.monotonic() < deadline:
        rows = client.call("RunStatusRequest", runId=run_id).active or []
        row = next((r for r in rows if list(map(text, r.card.segments)) == ["work"]), None)
        if row is not None and check(row):
            return row
        time.sleep(0.05)
    raise AssertionError(f"no matching active invocation: {rows}")


@pytest.mark.parametrize("child", [False, True])
def test_silent_cpu_bound_local_card_reports_growing_cpu_time(core_factory, project, tmp_path, child):
    """A local Card computing in silence, in its worker or in a child process,
    shows growing CPU time while its time since the last output grows, as a
    remote job does."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    Project(project).card(
        "work", {"type": "code", "sandbox": SCRIPTED_WORKER_SANDBOX},
        outputs="table Outputs {} root_type Outputs;", plan=mock_plan([str(project / "definitions/work")]),
        sources={"scenario.json": json.dumps({"actions": [
            {"op": "log", "text": "computing"}, {"op": "spin", "seconds": 12, "child": child}]})},
    ).save(client)
    run_id = run(client)
    first = active_row(client, run_id, lambda r: r.cpuMs is not None and r.outputIdleMs is not None)
    time.sleep(5)
    later = active_row(client, run_id, lambda r: True)
    assert later.cpuMs is not None and later.outputIdleMs > first.outputIdleMs
    # Sampled every two seconds: the busy Card added at least two of the five.
    assert later.cpuMs - first.cpuMs >= 2000, (first.cpuMs, later.cpuMs)
    assert wait_run(client, run_id).state == RunState.succeeded


def test_resident_worker_counts_cpu_time_from_its_call(core_factory, project, tmp_path):
    """A resident worker's earlier calls are not this call's CPU time."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    gate = Gate(tmp_path, "second")
    Project(project).card(
        "work", {"type": "code", "sandbox": SCRIPTED_WORKER_SANDBOX, "reuseWorker": True},
        outputs="table Outputs {} root_type Outputs;", plan=mock_plan([str(project / "definitions/work")]),
        sources={"scenario.json": json.dumps({"actions": [
            {"op": "spin", "seconds": 4, "call": 1}, {**gate.action(), "call": 2}]})},
    ).save(client)
    busy = run(client)
    assert active_row(client, busy, lambda r: (r.cpuMs or 0) >= 1000).cpuMs >= 1000
    assert wait_run(client, busy).state == RunState.succeeded
    idle = run(client)
    gate.wait()
    try:
        row = active_row(client, idle, lambda r: r.cpuMs is not None)
        time.sleep(2.5)
        later = active_row(client, idle, lambda r: True)
        assert row.cpuMs < 1000 and later.cpuMs < 1000, (row.cpuMs, later.cpuMs)
    finally:
        gate.release()
    assert wait_run(client, idle).state == RunState.succeeded
    record, = client.call("RunInvocationsRequest", runId=idle, card=path("work")).records
    assert record.workerReused


def test_local_cpu_time_includes_its_build_and_stays_in_history(core_factory, project, tmp_path):
    """The first member of a local invocation carries the CPU time of the Nix
    build done for it, the Card's call adds its own, and the last value stays
    with the finished invocation, also across a core restart. A later run that
    builds nothing counts its call only."""
    build_cpu = 40_000
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    Project(project).card(
        "work", {"type": "code", "sandbox": SCRIPTED_WORKER_SANDBOX},
        outputs="table Outputs {} root_type Outputs;",
        plan=mock_plan([str(project / "definitions/work")], buildCpuMs=build_cpu),
        sources={"scenario.json": json.dumps({"actions": [{"op": "spin", "seconds": 3}]})},
    ).save(client)
    built = run(client)
    assert active_row(client, built, lambda r: (r.cpuMs or 0) >= build_cpu).cpuMs >= build_cpu
    assert wait_run(client, built).state == RunState.succeeded

    def recorded(c, run_id):
        record, = c.call("RunInvocationsRequest", runId=run_id, card=path("work")).records
        return record.cpuMs

    # The call spun for three seconds: its last sample is part of the value.
    # A loaded runner gives the spin less CPU than its wall time (1.9 s seen).
    final = recorded(client, built)
    assert final is not None and final >= build_cpu + 1000, final
    daemon.stop()
    daemon.start()
    client = daemon.connect()
    assert recorded(client, built) == final
    detail = client.call("RunRecordRequest", runId=built, card=path("work"), invocation=1).record
    assert detail.summary.cpuMs == final

    again = run(client)
    assert wait_run(client, again).state == RunState.succeeded
    reused = recorded(client, again)
    assert reused is not None and 2000 <= reused < build_cpu, reused
