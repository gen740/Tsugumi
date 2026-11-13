"""Scratch lifetime, independent kept files and the cached space-management RPC."""

import os
import shutil
import time
from pathlib import Path

import pytest

from harness.fixtures import worker_project
from harness.observations import decode_scalar, read_output, read_pages
from harness.scenario import FILE_OUTPUT, run, wait_run
from tsugumi_client import new_id, path, text
from Tsugumi.Data.RunState import RunState


def usage_when(client, condition, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        usage = client.call("ProjectCacheUsageRequest")
        if usage.measuredAt is not None and condition(usage):
            return usage
        time.sleep(0.01)
    raise AssertionError(f"cache usage did not converge: {usage}")


def current(usage):
    return next(p for p in usage.projects if text(p.project) == text(usage.currentProject))


@pytest.mark.parametrize("record_io,retain", [(False, False), (True, False), (False, True), (True, True)])
def test_only_latest_run_scratch_remains_and_kept_files_survive(core_factory, project, tmp_path, record_io, retain):
    """A new run replaces scratch while recorded files and artifacts remain intact."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True,
                          args=["--run-scratch-dir", str(tmp_path / "scratch")])
    client = daemon.connect()
    payload = "large output\n" * 10000
    worker_project(client, project, tmp_path / "first", [
        {"op": "file", "port": "doc", "path": "report.txt", "text": payload},
    ], outputs=FILE_OUTPUT, recordIO=record_io, retainArtifact=retain, artifactOutput="doc")
    first = run(client)
    assert wait_run(client, first).state == RunState.succeeded
    usage_when(client, lambda u: current(u).scratchBytes == len(payload))
    saved = Path(decode_scalar(read_output(client, first, port="doc"))) if record_io else None

    # A separate package is fixture setup; saving uses the same public Card.
    worker_project(client, project, tmp_path / "second", [
        {"op": "file", "port": "doc", "path": "report.txt", "text": "small"},
    ], outputs=FILE_OUTPUT, recordIO=record_io, retainArtifact=retain, artifactOutput="doc")
    second = run(client)
    assert wait_run(client, second).state == RunState.succeeded
    usage_when(client, lambda u: current(u).scratchBytes == len("small"))
    if saved:
        assert saved.read_text() == payload
    artifacts = client.call("ArtifactListRequest", runId=first).artifacts
    assert len(artifacts) == int(retain)
    if retain:
        assert read_pages(client, "ArtifactReadRequest", artifactId=text(artifacts[0].artifactId)) == payload.encode()

    daemon.restart()
    client = daemon.connect()
    usage_when(client, lambda u: current(u).scratchBytes == len("small"))
    client.call("RunHistoryDeleteRequest", opId=new_id())
    daemon.restart()
    usage_when(daemon.connect(), lambda u: current(u).scratchBytes == 0)


def test_startup_removes_scratch_without_history(core_factory, tmp_path):
    """Leftover scratch without a run record is excluded after startup cleanup."""
    scratch = tmp_path / "scratch"
    orphan = scratch / "runs" / new_id()
    orphan.mkdir(parents=True)
    (orphan / "unfinished-output").write_bytes(b"orphaned scratch")
    daemon = core_factory(mocked_builds=True, args=["--run-scratch-dir", str(scratch)])
    usage_when(daemon.connect(), lambda u: current(u).scratchBytes == 0)


@pytest.mark.parametrize("include_content", [False, True])
def test_copied_history_reports_kept_file_location_or_missing_content(core_factory, project, tmp_path, include_content):
    """A copied history locates its own file copy or clearly marks missing content."""
    original = core_factory(mocked_builds=True, mocked_worker=True)
    client = original.connect()
    worker_project(client, project, tmp_path / "source", [
        {"op": "file", "port": "doc", "path": "result.txt", "text": "kept independently"},
    ], outputs=FILE_OUTPUT, recordIO=True)
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    evidence = read_output(client, run_id, port="doc")
    original.stop()
    copied = tmp_path / "copied"
    shutil.copytree(project, copied)
    if not include_content:
        shutil.rmtree(copied / ".tsugumi" / "runs")
    restored = core_factory(project_root=copied, mocked_builds=True).connect()
    value = restored.call("RunRecordRequest", runId=run_id, card=path("work")).record.outputs[0]
    assert value.fileMissing is not include_content
    assert read_output(restored, run_id, port="doc") == evidence
    if include_content:
        assert Path(text(value.filePath)).is_relative_to(copied)
        assert Path(text(value.filePath)).read_text() == "kept independently"
    else:
        assert not value.filePath


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
    assert rows["one"].lastUsed <= int((age + 1) * 1000)
    # Reading a cached report has no store traversal or mutation side effect.
    assert client.call("ProjectCacheUsageRequest").measuredAt == usage.measuredAt
    client.call("ProjectCacheClearRequest", opId=new_id())
    usage_when(client, lambda u: u.buildBytes == size)  # another project's roots stay


def test_more_than_two_builds_remain_registered(core_factory, project, tmp_path):
    """Building a third definition does not expire earlier build registrations."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    previous = 0
    for index in range(4):
        worker_project(client, project, tmp_path / str(index), [
            {"op": "output", "port": "out", "type": "string", "value": str(index)},
        ])
        run_id = run(client)
        assert wait_run(client, run_id).state == RunState.succeeded
        usage = usage_when(client, lambda u: u.buildBytes is not None and u.buildBytes > previous)
        previous = usage.buildBytes
    daemon.restart()
    usage_when(daemon.connect(), lambda u: u.buildBytes == previous)
