"""Untrusted paths cannot escape the project through its public RPC operations."""

import json

import pytest

from harness.fixtures import worker_project
from harness.observations import read_pages
from harness.scenario import Project, run, wait_run
from projects import builtin_project, put
from tsugumi_client import RpcFailure, change, new_id, path, text
from Tsugumi.Data.RunState import RunState

DIRECTORY_OUTPUT = ('attribute "tsugumi"; table Directory { value:string; } '
                    'table Outputs { doc:Directory (tsugumi:"directory"); } root_type Outputs;')


@pytest.mark.parametrize("target,kind", [
    ("../outside.txt", "invalid_path"),
    ("definitions/../../outside.txt", "invalid_path"),
    (".tsugumi/forbidden.txt", "permission_denied"),
    ("absolute", "invalid_path"),
], ids=["parent-path", "nested-parent-path", "private-state-path", "absolute-path"])
def test_a_source_save_cannot_write_outside_the_project(core_factory, project, tmp_path, target, kind):
    """An invalid source path rejects the whole save and preserves the current project."""
    client = core_factory().connect()
    Project(project).card("work", {"template": "fork"}, sources={"keep.txt": "original"}).save(client)
    before = client.call("ProjectInspectRequest")
    if target == "absolute":
        target = str(tmp_path / "outside.txt")
    with pytest.raises(RpcFailure) as failure:
        client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test", changes=[
            change("SetLayout", json=json.dumps({"cards": {"work": {"x": 999, "y": 999}}})),
            change("PutSource", path=target, bytes=b"untrusted"),
        ])
    assert failure.value.kind == kind
    after = client.call("ProjectInspectRequest")
    assert after.rev == before.rev
    assert text(after.layoutJson) == text(before.layoutJson)
    assert read_pages(client, "ProjectFileReadRequest", path="definitions/work/keep.txt") == b"original"


@pytest.mark.parametrize("target", ["../secret.txt", "definitions/../../secret.txt", "/etc/passwd"])
def test_project_reads_refuse_paths_outside_the_project(core_factory, target):
    """Project file reads and stats reject traversal and absolute paths."""
    client = core_factory().connect()
    for request in ("ProjectFileReadRequest", "ProjectFileStatRequest"):
        with pytest.raises(RpcFailure) as failure:
            client.call(request, path=target)
        assert failure.value.kind == "invalid_path"
    assert client.call("ProjectInspectRequest").rev == 0


def test_a_source_symlink_cannot_overwrite_another_project(core_factory, project, tmp_path):
    """A source symlink aimed at another project is rejected without changing its source."""
    client = core_factory().connect()
    Project(project).card("work", {"template": "fork"}).save(client)
    outside = tmp_path / "outside"
    builtin_project(outside)
    put(outside / "definitions/src/keep.txt", "protected")
    (project / "definitions/work/escape").symlink_to(outside / "definitions/src", target_is_directory=True)
    before = client.call("ProjectInspectRequest")
    with pytest.raises(RpcFailure) as failure:
        client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test", changes=[
            change("PutSource", path="definitions/work/escape/keep.txt", bytes=b"overwritten"),
        ])
    assert failure.value.kind == "invalid_path"
    assert client.call("ProjectInspectRequest").rev == before.rev
    observer = core_factory(project_root=outside).connect()
    assert read_pages(observer, "ProjectFileReadRequest", path="definitions/src/keep.txt") == b"protected"


def test_artifact_paths_and_history_cleanup_stay_inside_the_project(core_factory, project, tmp_path):
    """Invalid artifact paths are rejected and history deletion preserves other projects."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    worker_project(client, project, tmp_path, [
        {"op": "directory", "port": "doc", "path": "result", "files": {"keep.txt": "retained"}},
    ], outputs=DIRECTORY_OUTPUT, retainArtifact=True, artifactOutput="doc")
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    artifact = client.call("ArtifactListRequest", runId=run_id, card=path("work")).artifacts[0]
    artifact_id = text(artifact.artifactId)

    for member in ("../secret.txt", "nested/../../secret.txt", "/etc/passwd"):
        with pytest.raises(RpcFailure) as failure:
            client.call("ArtifactReadRequest", artifactId=artifact_id, member=member)
        assert failure.value.kind == "invalid_path"
    with pytest.raises(RpcFailure) as failure:
        client.call("ArtifactReadRequest", artifactId="../../../outside")
    assert failure.value.kind == "invalid_request"
    with pytest.raises(RpcFailure) as failure:
        client.call("RunInvocationsRequest", runId="../../../outside", card=path("work"))
    assert failure.value.kind == "invalid_request"
    assert read_pages(client, "ArtifactReadRequest", artifactId=artifact_id, member="keep.txt") == b"retained"

    outside = tmp_path / "outside"
    builtin_project(outside)
    put(outside / "definitions/src/keep.txt", "protected")
    client.call("RunHistoryDeleteRequest", opId=new_id())
    with pytest.raises(RpcFailure) as failure:
        client.call("ArtifactListRequest", runId=run_id)
    assert failure.value.kind == "not_found"
    with pytest.raises(RpcFailure) as failure:
        client.call("ArtifactReadRequest", artifactId=artifact_id, member="keep.txt")
    assert failure.value.kind == "not_found"
    observer = core_factory(project_root=outside).connect()
    assert read_pages(observer, "ProjectFileReadRequest", path="definitions/src/keep.txt") == b"protected"
