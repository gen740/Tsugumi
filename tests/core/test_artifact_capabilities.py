"""Artifact capability lifetimes through the Core's public RPC."""

import sqlite3
from uuid import UUID

import pytest

from harness.fixtures import add_worker, worker_project
from harness.observations import decode_scalar, read_output
from harness.scenario import FILE_OUTPUT, Gate, Project, run, wait_run
from Tsugumi.Data.ArtifactState import ArtifactState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState
from tsugumi_client import RpcFailure, path, text


def test_artifact_ids_are_unique_random_uuids(core_factory, project, tmp_path):
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    count = 64
    worker_project(client, project, tmp_path, [
        {"op": "file", "port": "doc", "path": "report.txt", "text": str(i)}
        for i in range(count)
    ], outputs=FILE_OUTPUT)
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    artifacts = client.call("ArtifactListRequest", runId=run_id).artifacts
    ids = {UUID(text(artifact.artifactId)) for artifact in artifacts}
    assert len(ids) == count
    assert all(key.version == 4 and key.variant == "specified in RFC 4122" for key in ids)
    assert [text(row.label).split(".")[1] for row in artifacts] == list(map(str, range(count)))
    # A sequential counter or a shared per-invocation prefix must not be
    # encoded anywhere in these UUIDs.
    assert len({key.bytes[:6] for key in ids}) == count
    assert len({key.bytes[-6:] for key in ids}) == count


@pytest.mark.parametrize("interrupted", [False, True], ids=["run-end", "restart"])
def test_transient_artifact_opens_without_a_port_id_then_expires(
    core_factory, project, tmp_path, interrupted
):
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    gate = Gate(tmp_path, "opened")
    key_file = tmp_path / "uuid"
    graph = Project(project)
    outputs = FILE_OUTPUT.replace(
        "table Outputs { doc:tsugumi.Artifact; }",
        "table Ready { value:bool; } table Outputs { doc:tsugumi.Artifact; ready:Ready; }",
    )
    add_worker(graph, "source", tmp_path / "source", [
        {"op": "file", "port": "doc", "path": "scratch.txt", "text": "scratch",
         "keep": False, "uuid_file": str(key_file)},
        {"op": "output", "port": "ready", "type": "bool", "value": True},
    ], outputs=outputs)
    add_worker(graph, "sink", tmp_path / "sink", [
        {"op": "open_artifact", "uuid_file": str(key_file),
         "member": "scratch.txt", "port": "out"},
        gate.action(),
    ], inputs=[{"name": "ready"}])
    graph.connect("source", "ready", "sink", "ready").save(client)
    run_id = run(client, "source", propagate=True)
    gate.wait()
    artifact_id = key_file.read_text()
    [artifact] = client.call("ArtifactListRequest", artifactIds=[artifact_id]).artifacts
    assert text(artifact.artifactId) == artifact_id
    assert not artifact.keep
    directory = project / ".tsugumi/artifacts" / run_id / "source/@1/0"
    assert (directory / "scratch.txt").read_text() == "scratch"
    if interrupted:
        daemon.restart(crash=True)
        client = daemon.connect()
        assert client.call("RunStatusRequest", runId=run_id).state == RunState.failed
    else:
        gate.release()
        assert wait_run(client, run_id).state == RunState.succeeded
        assert decode_scalar(read_output(client, run_id, card="sink"), "string") == "scratch"
    assert not directory.exists()
    assert not client.call("ArtifactListRequest", artifactIds=[artifact_id]).artifacts
    assert not client.call("RunHistoryRequest", artifactsOnly=True).runs
    with sqlite3.connect(project / ".tsugumi/runs.sqlite") as db:
        assert not db.execute("SELECT 1 FROM artifacts WHERE run_id=?", (run_id,)).fetchone()
    record = client.call("RunRecordRequest", runId=run_id, card=path("source")).record
    doc = next(value for value in record.outputs if text(value.name) == "doc")
    [expired] = doc.artifacts
    assert text(expired.id) == artifact_id and expired.state == ArtifactState.unavailable
    assert expired.sizeBytes is None and expired.label is None
    with pytest.raises(RpcFailure) as gone:
        client.call("ArtifactReadRequest", artifactId=artifact_id, member="scratch.txt")
    assert gone.value.kind == "not_found"
    # The interrupted consumer never committed its saved inputs.
    if interrupted:
        return
    rerun = run(client, "sink", propagate=False)
    assert wait_run(client, rerun).state == RunState.failed
    failed = client.call("RunRecordRequest", runId=rerun, card=path("sink")).record
    assert failed.summary.failure.kind == FailureKind.input_missing
    assert "artifact unavailable" in text(failed.summary.failure.diagnostic)
