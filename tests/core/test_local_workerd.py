"""Local slots belong to workerd, and end with their starting core."""

import json
import sys

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
