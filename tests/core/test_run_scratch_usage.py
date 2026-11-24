"""Scratch lifetime, independent kept files and the cached space-management RPC."""

import os
import time
from pathlib import Path

import pytest

from harness.fixtures import worker_project
from harness.observations import read_pages
from harness.scenario import FILE_OUTPUT, Gate, run, wait_run
from tsugumi_client import RpcFailure, new_id, path, text
from Tsugumi.Data.RunState import RunState


def usage_when(client, condition):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        usage = client.call("ProjectCacheUsageRequest")
        if usage.measuredAt is not None and condition(usage):
            return usage
        time.sleep(0.01)
    pytest.fail("project cache usage did not reach the expected state")


def current(usage):
    return next(p for p in usage.projects if text(p.project) == text(usage.currentProject))


def write_run_scratch(scratch, run_id, contents):
    directory = scratch / "runs" / run_id
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "scratch.txt").write_bytes(contents)


def test_past_run_scratch_stays_until_explicit_deletion(core_factory, project, tmp_path):
    """A new run and restart keep scratch; explicit deletion leaves independent copies."""
    scratch = tmp_path / "scratch"
    daemon = core_factory(mocked_builds=True, mocked_worker=True,
                          args=["--run-scratch-dir", str(scratch)])
    client = daemon.connect()
    payload = "large output\n" * 10000
    first_scratch = b"first run scratch"
    second_scratch = b"second run scratch"
    worker_project(client, project, tmp_path / "first", [
        {"op": "file", "port": "doc", "path": "report.txt", "text": payload},
    ], outputs=FILE_OUTPUT, recordIO=True)
    first = run(client)
    assert wait_run(client, first).state == RunState.succeeded
    usage_when(client, lambda u: current(u).scratchBytes == 0)
    write_run_scratch(scratch, first, first_scratch)
    usage_when(client, lambda u: current(u).scratchBytes == len(first_scratch))

    # A separate package is fixture setup; saving uses the same public Card.
    worker_project(client, project, tmp_path / "second", [
        {"op": "file", "port": "doc", "path": "report.txt", "text": "small"},
    ], outputs=FILE_OUTPUT, recordIO=True)
    second = run(client)
    assert wait_run(client, second).state == RunState.succeeded
    write_run_scratch(scratch, second, second_scratch)
    total_scratch = len(first_scratch) + len(second_scratch)
    usage_when(client, lambda u: current(u).scratchBytes == total_scratch)
    artifact, = client.call("ArtifactListRequest", runId=first).artifacts
    artifact_id = text(artifact.artifactId)

    daemon.restart()
    client = daemon.connect()
    usage_when(client, lambda u: current(u).scratchBytes == total_scratch)
    candidates = usage_when(client, lambda u: len(current(u).runScratch) >= 2)
    assert text(candidates.protectedRun) == second
    sizes = {text(p.path): p.bytes for p in current(candidates).runScratch}
    assert sizes[f"runs/{first}"] == len(first_scratch)
    assert sizes[f"runs/{second}"] == len(second_scratch)
    for paths, kind in [([f"runs/{first}", f"runs/{second}"], "busy"),
                        ([f"runs/{first}", f"runs/0{second}"], "invalid_request"),
                        ([f"runs/{first}", "../outside"], "invalid_request")]:
        with pytest.raises(RpcFailure) as failure:
            client.call("RunScratchDeleteRequest", opId=new_id(), paths=paths)
        assert failure.value.kind == kind
        assert (scratch / "runs" / first).exists()
    op_id = new_id()
    deleted = client.call("RunScratchDeleteRequest", opId=op_id, paths=[f"runs/{first}"])
    assert [text(p) for p in deleted.deleted] == [f"runs/{first}"]
    assert not deleted.retained
    assert deleted.freedBytes == len(first_scratch)
    assert client.call("RunScratchDeleteRequest", opId=op_id, paths=[f"runs/{first}"]).freedBytes == len(first_scratch)
    usage_when(client, lambda u: current(u).scratchBytes == len(second_scratch))
    assert read_pages(client, "ArtifactReadRequest", artifactId=artifact_id, member="report.txt") == payload.encode()
    assert client.call("RunRecordRequest", runId=first, card=path("work")).record
    client.call("RunHistoryDeleteRequest", opId=new_id())
    daemon.restart()
    usage_when(daemon.connect(), lambda u: current(u).scratchBytes == 0)


def test_startup_keeps_scratch_without_history(core_factory, tmp_path):
    """Orphan scratch is reported and removed only by an explicit selection."""
    scratch = tmp_path / "scratch"
    orphan = scratch / "runs" / "97"
    orphan.mkdir(parents=True)
    (orphan / "unfinished-output").write_bytes(b"orphaned scratch")
    daemon = core_factory(mocked_builds=True, args=["--run-scratch-dir", str(scratch)])
    client = daemon.connect()
    usage_when(client, lambda u: current(u).scratchBytes == len(b"orphaned scratch"))
    assert (orphan / "unfinished-output").read_bytes() == b"orphaned scratch"
    client.call("RunScratchDeleteRequest", opId=new_id(), paths=[f"runs/{orphan.name}"])
    assert not orphan.exists()


def test_failed_acceptance_preserves_previous_scratch(core_factory, project, tmp_path):
    scratch = tmp_path / "scratch"
    daemon = core_factory(mocked_builds=True, mocked_worker=True,
                          args=["--run-scratch-dir", str(scratch)])
    client = daemon.connect()
    worker_project(client, project, tmp_path, [
        {"op": "file", "port": "doc", "path": "report.txt", "text": "previous output"},
    ], outputs=FILE_OUTPUT)
    first = run(client)
    assert wait_run(client, first).state == RunState.succeeded
    write_run_scratch(scratch, first, b"previous run scratch")
    before = {p.relative_to(scratch): p.read_bytes()
              for p in scratch.rglob("*") if p.is_file()}
    daemon.restart(fault="run_acceptance")
    client = daemon.connect()
    with pytest.raises(RpcFailure) as failure:
        run(client)
    assert failure.value.kind == "persistence_failed"
    assert {p.relative_to(scratch): p.read_bytes()
            for p in scratch.rglob("*") if p.is_file()} == before
    assert [text(r.status.runId) for r in client.call("RunHistoryRequest").runs] == [first]
    assert sorted(p.name for p in (scratch / "runs").iterdir()) == [first]


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
        "--usage-scratch-dir", str(tmp_path / "scratch"), "--run-scratch-dir", str(tmp_path / "scratch" / "current"),
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


def test_scratch_deletion_refuses_active_run(core_factory, project, tmp_path):
    scratch = tmp_path / "scratch"
    orphan = scratch / "runs" / "97"
    orphan.mkdir(parents=True)
    (orphan / "evidence").write_text("keep while running")
    daemon = core_factory(mocked_builds=True, mocked_worker=True,
                          args=["--run-scratch-dir", str(scratch)])
    client = daemon.connect()
    gate = Gate(tmp_path, "running")
    worker_project(client, project, tmp_path, [gate.action(),
        {"op": "output", "port": "out", "type": "string", "value": "done"},
    ])
    run_id = run(client)
    gate.wait()
    try:
        with pytest.raises(RpcFailure) as failure:
            client.call("RunScratchDeleteRequest", opId=new_id(), paths=[f"runs/{orphan.name}"])
        assert failure.value.kind == "busy"
        assert (orphan / "evidence").read_text() == "keep while running"
    finally:
        gate.release()
    assert wait_run(client, run_id).state == RunState.succeeded


def test_unreadable_scratch_is_listed_and_explicitly_deletable(core_factory, tmp_path):
    """One chmod-000 folder neither hides peers nor gates the deletion RPC."""
    if os.geteuid() == 0:
        pytest.skip("permission denial requires a non-root user")
    scratch = tmp_path / "scratch"
    locked = scratch / "runs" / "97"
    other = scratch / "runs" / "98"
    (locked / "private").mkdir(parents=True)
    other.mkdir(parents=True)
    (locked / "private" / "value").write_bytes(b"unknown")
    (other / "value").write_bytes(b"known")
    (locked / "private").chmod(0)
    try:
        client = core_factory(mocked_builds=True, args=["--run-scratch-dir", str(scratch)]).connect()
        # Startup creates nothing in run scratch, so only these are listed.
        expected = {f"runs/{locked.name}", f"runs/{other.name}"}
        usage = usage_when(client, lambda u: {text(p.path) for p in current(u).runScratch} == expected)
        folders = {text(p.path): p for p in current(usage).runScratch}
        assert folders[f"runs/{locked.name}"].sizeUnknown
        assert not folders[f"runs/{other.name}"].sizeUnknown
        assert current(usage).scratchBytes is None
        result = client.call("RunScratchDeleteRequest", opId=new_id(), paths=list(folders))
        assert set(map(text, result.deleted)) == set(folders)
        assert not result.retained
        assert not locked.exists() and not other.exists()
    finally:
        if locked.exists():
            (locked / "private").chmod(0o700)
