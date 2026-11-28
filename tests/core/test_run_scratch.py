"""Run scratch is a working folder: it is erased when each invocation ends.

Run data (records, values, logs and kept artifacts) stays until a person
deletes it. Scratch is tmp and is cleaned up by design; to keep files, a Card
mounts a host folder (grants.hostPaths).
"""

import os
import time
from pathlib import Path

import pytest

from harness.fixtures import add_worker, worker_project
from harness.observations import decode_scalar, read_output, read_pages
from harness.scenario import FILE_OUTPUT, Gate, Project, run, wait_run
from tsugumi_client import new_id, path, text
from Tsugumi.Data.RunState import RunState


def usage_when(client, condition):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        usage = client.call("ProjectCacheUsageRequest")
        if usage.measuredAt is not None and condition(usage):
            return usage
        time.sleep(0.01)
    pytest.fail("project cache usage did not reach the expected state")


def gone_soon(file: Path):
    # A fresh worker is retired, and its folders removed, as its call ends.
    deadline = time.monotonic() + 15
    while file.exists() and time.monotonic() < deadline:
        time.sleep(0.02)
    return not file.exists()


@pytest.mark.parametrize("ending", ["succeeded", "failed", "cancelled"])
def test_scratch_is_erased_when_the_invocation_ends(core_factory, project, tmp_path, ending):
    scratch = tmp_path / "scratch"
    daemon = core_factory(mocked_builds=True, mocked_worker=True,
                          args=["--run-scratch-dir", str(scratch)])
    client = daemon.connect()
    gate = Gate(tmp_path, "running")
    record = tmp_path / "tmp-file"
    last = ({"op": "failure", "message": "the Card failed"} if ending == "failed"
            else {"op": "output", "port": "out", "type": "string", "value": "done"})
    worker_project(client, project, tmp_path, [
        {"op": "tmp_file", "name": "working.txt", "record": str(record)},
        gate.action(), last,
    ])
    run_id = run(client)
    gate.wait()
    working = Path(record.read_text())
    if not working.exists():
        # A Linux sandbox mounts the call's scratch tmp folder as the Card's
        # /tmp: the path it reports is its own view, not this host's.
        working, = (found for root in (scratch, daemon.directory)
                    for found in root.rglob(f"tmp/{working.name}"))
    assert working.read_text() == "working file"
    # Stands for anything the core put in the run's scratch for this call.
    leftover = scratch / "runs" / run_id / "work" / "work" / "1" / "leftover"
    leftover.parent.mkdir(parents=True, exist_ok=True)
    leftover.write_text("intermediate")
    if ending == "cancelled":
        client.call("RunCancelRequest", runId=run_id)
    gate.release()
    status = wait_run(client, run_id)
    assert status.state == getattr(RunState, ending)
    assert not (scratch / "runs" / run_id).exists()
    assert gone_soon(working)
    # The record of the run stays.
    assert client.call("RunRecordRequest", runId=run_id, card=path("work")).record
    if ending == "succeeded":
        assert decode_scalar(read_output(client, run_id), "string") == "done"


def test_kept_artifacts_survive_the_erased_scratch(core_factory, project, tmp_path):
    scratch = tmp_path / "scratch"
    daemon = core_factory(mocked_builds=True, mocked_worker=True,
                          args=["--run-scratch-dir", str(scratch)])
    client = daemon.connect()
    worker_project(client, project, tmp_path, [
        {"op": "file", "port": "doc", "path": "report.txt", "text": "kept report"},
    ], outputs=FILE_OUTPUT, recordIO="on")
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    assert not (scratch / "runs" / run_id).exists()
    artifact, = client.call("ArtifactListRequest", runId=run_id).artifacts
    artifact_id = text(artifact.artifactId)
    assert read_pages(client, "ArtifactReadRequest", artifactId=artifact_id,
                      member="report.txt") == b"kept report"
    daemon.restart()
    client = daemon.connect()
    assert read_pages(client, "ArtifactReadRequest", artifactId=artifact_id,
                      member="report.txt") == b"kept report"


def test_rerun_uses_recorded_inputs_without_old_scratch(core_factory, project, tmp_path):
    scratch = tmp_path / "scratch"
    daemon = core_factory(mocked_builds=True, mocked_worker=True,
                          args=["--run-scratch-dir", str(scratch)])
    client = daemon.connect()
    graph = Project(project)
    add_worker(graph, "source", tmp_path / "source", [
        {"op": "output", "port": "out", "type": "string", "value": "recorded input"},
    ])
    add_worker(graph, "work", tmp_path / "work", [
        {"op": "echo", "input": "in", "port": "out"},
    ], inputs=[{"name": "in"}])
    graph.connect("source", "out", "work", "in").save(client)
    first = run(client, "source")
    assert wait_run(client, first).state == RunState.succeeded
    expected = read_output(client, first)
    runs = scratch / "runs"
    assert not runs.exists() or not any(runs.iterdir())
    daemon.restart()
    client = daemon.connect()
    again = run(client, "work", propagate=False)
    status = wait_run(client, again)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    assert read_output(client, again) == expected


def test_startup_collects_interrupted_artifacts_delete_markers_and_scratch(
    core_factory, project, tmp_path
):
    scratch = tmp_path / "scratch"
    artifacts = project / ".tsugumi" / "artifacts"
    daemon = core_factory(mocked_builds=True, mocked_worker=True,
                          args=["--run-scratch-dir", str(scratch)])
    client = daemon.connect()
    gate = Gate(tmp_path, "running")
    worker_project(client, project, tmp_path, [gate.action()])
    interrupted = run(client)
    gate.wait()
    owner = artifacts / interrupted
    orphan = [
        owner / "outer" / "inner" / "@1" / "0",
        artifacts / ".delete-97",
        owner / "card" / "@1" / ".delete-68bbf780-8307-4fcd-870d-8559ced179c6",
    ]
    leftover_scratch = [scratch / "runs" / "97", scratch / "runs" / "manual"]
    unrelated = [
        artifacts / "97" / "card" / "@1" / "0",
        owner / "card" / "@01" / "0",
        owner / "@1" / "0",
        artifacts / ".delete-manual",
        project / ".tsugumi" / "other" / "97",
    ]
    for directory in orphan + leftover_scratch + unrelated:
        directory.mkdir(parents=True)
        (directory / "file").write_bytes(b"example")
    link = owner / "card" / "@1" / "1"
    link.parent.mkdir(parents=True, exist_ok=True)
    link.symlink_to(unrelated[-1], target_is_directory=True)
    daemon.restart(crash=True)
    daemon.connect()
    assert all(not path.exists() for path in orphan + leftover_scratch)
    assert all((path / "file").read_bytes() == b"example" for path in unrelated)
    assert link.is_symlink()


def test_usage_deduplicates_shared_roots_and_keeps_old_roots(core_factory, tmp_path):
    """Old project roots survive startup; shared outputs count once in the total."""
    roots = tmp_path / "roots"
    shared = tmp_path / "build-product"
    shared.write_bytes(b"shared deterministic build result")
    age = time.time() - 90 * 86400
    for project in ["one", "two"]:
        folder = roots / project
        folder.mkdir(parents=True)
        link = folder / "result"
        link.symlink_to(shared)
        os.utime(link, (age, age), follow_symlinks=False)
        os.utime(folder, (age, age))
    daemon = core_factory(mocked_builds=True, args=[
        "--gcroots-dir", str(roots / "current"), "--usage-roots-dir", str(roots),
        "--run-scratch-dir", str(tmp_path / "scratch" / "current"),
    ])
    client = daemon.connect()
    size = shared.stat().st_size
    usage = usage_when(client, lambda u: u.buildBytes == size)
    rows = {text(p.project): p for p in usage.projects}
    assert rows["one"].buildBytes == rows["two"].buildBytes == size
    client.call("ProjectCacheClearRequest", opId=new_id())
    usage_when(client, lambda u: u.buildBytes == size)  # another project's roots stay


def test_more_than_two_builds_remain_registered(core_factory, project, tmp_path):
    """Building a third definition does not expire earlier build registrations."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    previous = 0
    for index in range(3):
        worker_project(client, project, tmp_path / str(index), [
            {"op": "output", "port": "out", "type": "string", "value": str(index)},
        ])
        run_id = run(client)
        assert wait_run(client, run_id).state == RunState.succeeded
        usage = usage_when(client, lambda u: u.buildBytes is not None and u.buildBytes > previous)
        previous = usage.buildBytes
    daemon.restart()
    usage_when(daemon.connect(), lambda u: u.buildBytes == previous)
